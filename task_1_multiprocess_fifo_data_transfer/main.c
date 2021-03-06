#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>

/* Value to identify pair */
struct ident_t {
	pid_t pid;
};

const char FIFO_QUEUE_PATH[] = "queue.fifo";
const char FIFO_CHANNEL_PATH_PREFIX[] = "channel.";
const int FIFO_CHANNEL_PATH_MAX = sizeof(FIFO_CHANNEL_PATH_PREFIX) +
    				  sizeof(struct ident_t) * 2;
const mode_t FIFO_MODE = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
    			 S_IROTH | S_IWOTH;

const unsigned FIFO_MAX_SLEEP_US = ((unsigned)2) << 22;
const unsigned FIFO_MIN_SLEEP_US = ((unsigned)2) << 2;

/* POSIX-atomic */
const int FDTOFD_CPY_BUF_SIZE = 512;

ssize_t buftofd_cpy(int fd_out, char *buf, size_t count)
{
	ssize_t tmp = 1;
	size_t len = count;
	while (len && tmp) {
		tmp = write(fd_out, buf, len);
		if (tmp == -1) {
			if (errno != EINTR)
				return -1;
		} else {
			len -= tmp;
			buf += tmp;
		}
	}
	return count - len;
}

ssize_t fdtofd_cpy(int fd_out, int fd_in)
{
	char *buf = malloc(FDTOFD_CPY_BUF_SIZE);
	if (buf == NULL)
		return -1;
	ssize_t sumary = 0;
	while (1) {
		ssize_t len = read(fd_in, buf, FDTOFD_CPY_BUF_SIZE);
		if (len == -1) {
			if (errno == EINTR) {
				errno = 0;
				continue;
			}
			free(buf);
			return -1;
		} else if (len == 0)
			break;

		ssize_t tmp = buftofd_cpy(fd_out, buf, len);
		if (tmp == -1) {
			free(buf);
			return -1;
		}
		sumary += tmp;
		if (tmp != len)
			break;
	}
	free(buf);
	return sumary;
}

struct ident_t get_id()
{
	struct ident_t id;
	id.pid = getpid();
	return id;
}

int get_channel_path(char **buf_p, struct ident_t id)
{
	*buf_p = malloc(sizeof(char) * FIFO_CHANNEL_PATH_MAX);
	if (buf_p == NULL)
		return -1;
	return sprintf(*buf_p, "%s%jx\0",
		       FIFO_CHANNEL_PATH_PREFIX, (uintmax_t) id.pid);
}

int init_channel(char *path, mode_t mode)
{
	int fd;
	// Try to create fifo
	if (mkfifo(path, mode) != -1)
		return 0;
	if (errno != EEXIST) {
		perror("Error: mkfifo");
		return -1;
	}
	return 0;
}

/* @note This is critical section. */
int receiver_wait_byte(int fd)
{
	unsigned int delay = FIFO_MIN_SLEEP_US;
	char msg;
	int tmp;
	for (; delay <= FIFO_MAX_SLEEP_US; delay *= 2) {
		tmp = read(fd, &msg, sizeof(msg));
		if (tmp > 0) {
			return 1;
		}
		if (tmp == -1 && errno != EAGAIN) {
			return -1;
		}
		usleep(delay);
	}
	return 0;
}

#define ERR_HANDLER(str_msg)			\
do {						\
	fprintf(stderr, "Error: " str_msg);	\
	fprintf(stderr, "Line: %d\n", __LINE__);\
	unlink(channel_path);			\
	free(channel_path);			\
	return -1;				\
} while (0)

#define ERR_HANDLER_PERR(str_msg)		\
do {						\
	perror("Error: " str_msg);		\
	fprintf(stderr, "Line: %d\n", __LINE__);\
	unlink(channel_path);			\
	free(channel_path);			\
	return -1;				\
} while (0)

#define ERR_PERR_RET(str_msg)			\
do {						\
	perror("Error: " str_msg);		\
	return -1;				\
} while (0)

int receiver()
{
	/* Prepare and open channel */
	struct ident_t id = get_id();
	char *channel_path;
	get_channel_path(&channel_path, id);
	if (init_channel(channel_path, FIFO_MODE) == -1)
		ERR_HANDLER("init channel");

	int fd_channel = open(channel_path, O_RDONLY | O_NONBLOCK);
	if (fd_channel == -1)
		ERR_HANDLER_PERR("open channel");

	/* Create/open queue and write id */
	if (mkfifo(FIFO_QUEUE_PATH, FIFO_MODE) == -1 && errno != EEXIST)
		ERR_HANDLER_PERR("mkfifo queue");

	int fd_queue = open(FIFO_QUEUE_PATH, O_RDWR);
	if (fd_queue == -1)
		ERR_HANDLER_PERR("open queue");

	if (write(fd_queue, &id, sizeof(id)) != sizeof(id))
		ERR_HANDLER_PERR("write id to queue");

	/* Wait for msg from sender */
	if (receiver_wait_byte(fd_channel) != 1)
		ERR_HANDLER("It seems the sender is dead\n");

	/* Set new flags, write data, close file */
	if (fcntl(fd_channel, F_SETFL, O_RDONLY) == -1)
		ERR_HANDLER_PERR("fcntl channel");

	/* Send data */
	if (fdtofd_cpy(STDOUT_FILENO, fd_channel) == -1)
		ERR_HANDLER("copy from channel to stdout");

	/* Close fd */
	if (close(fd_channel) == -1)
		ERR_HANDLER_PERR("close channel");
	if (close(fd_queue) == -1)
		ERR_HANDLER_PERR("close queue");

	unlink(channel_path);
	free(channel_path);
	return 0;
}

int sender(const char *inp_path)
{
	/* Open input file */
	int fd_inp = open(inp_path, O_RDONLY);
	if (fd_inp == -1)
		ERR_PERR_RET("open input file");

	/* Create/open queue */
	if (mkfifo(FIFO_QUEUE_PATH, FIFO_MODE) == -1 && errno != EEXIST)
		ERR_PERR_RET("mkfifo queue");
	int fd_queue = open(FIFO_QUEUE_PATH, O_RDWR);
	if (fd_queue == -1)
		ERR_PERR_RET("open queue");

	/* Read id from queue */
	struct ident_t id;
	if (read(fd_queue, &id, sizeof(id)) != sizeof(id))
		ERR_PERR_RET("read id from queue");

	/* Open channel by id */
	char *channel_path;
	get_channel_path(&channel_path, id);
	int fd_channel = open(channel_path, O_NONBLOCK | O_WRONLY);
	if (fd_channel == -1)
		ERR_HANDLER_PERR("It seems the receiver is dead");

	/* Set new flags */
	if (fcntl(fd_channel, F_SETFL, O_WRONLY) == -1)
		ERR_HANDLER_PERR("fcntl channel");

	/* Send msg that sender is ready */
	char msg = 1;
	if (buftofd_cpy(fd_channel, &msg, sizeof(msg)) == -1)
		ERR_HANDLER("send sync-msg");

	/* Send data */
	if (fdtofd_cpy(fd_channel, fd_inp) == -1)
		ERR_HANDLER("copy from input to channel");

	/* Close fd */
	if (close(fd_channel) == -1)
		ERR_HANDLER_PERR("close channel");
	if (close(fd_inp) == -1)
		ERR_HANDLER_PERR("close input file");
	if (close(fd_queue) == -1)
		ERR_HANDLER_PERR("close queue");

	unlink(channel_path);
	free(channel_path);
	return 0;
}

#undef ERR_HANDLER
#undef ERR_HANDLER_PERR
#undef ERR_PERR_RET

int main(int argc, char *argv[])
{
	if (argc == 2) {
		if (sender(argv[1]) == -1)
			fprintf(stderr, "Error: Sender failed\n");
	} else if (argc == 1) {
		if (receiver() == -1)
			fprintf(stderr, "Error: Receiver failed\n");
	} else {
		fprintf(stderr, "Error: wrong argument list\n");
	}
	return EXIT_SUCCESS;
}
