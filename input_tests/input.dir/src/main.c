#include <errno.h>
#include <fcntl.h>
#include <input_event.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(struct input_event) == 8, "input event layout");

static void print_text(const char *text) {
    write(1, text, strlen(text));
}

static void print_number(int32_t value) {
    char buffer[12];
    unsigned pos = sizeof(buffer);
    int64_t number = value;

    if (number < 0) number = -number;

    do {
        buffer[--pos] = '0' + number % 10;
        number /= 10;
    } while (number != 0);

    if (value < 0) buffer[--pos] = '-';

    write(1, buffer + pos, sizeof(buffer) - pos);
}

static int fail(const char *operation) {
    int error = errno;
    print_text(operation);
    print_text(" failed, errno=");
    print_number(error);
    puts("");
    return 1;
}

static void print_event(const struct input_event *event) {
    const char *kind =
        event->type == 1 ? "key" :
        event->type == 2 ? "button" :
        event->type == 3 ? "motion" :
        event->type == 4 ? "scroll" : "unknown";

    print_text(kind);
    print_text(" code=");
    print_number(event->code);
    print_text(" value=");
    print_number(event->value);
    puts("");
}

int main(void) {
    int fd = open("/dev/input0", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return fail("open");

    struct pollfd watched = {
        .fd = fd,
        .events = POLLIN,
        .revents = 0
    };

    puts("Input test ready. Type, move, and click in VNC.");
    puts("Release Escape to finish.");

    int running = 1;

    while (running) {
        int ready = poll(&watched, 1, 1000);

        if (ready < 0) {
            if (errno == EINTR) continue;
            return fail("poll");
        }
        if (ready == 0) continue; // Timeout; try again.

        if (watched.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            puts("Input device reported an error.");
            return 1;
        }
        if (!(watched.revents & POLLIN)) continue;

        struct input_event events[32];
        ssize_t bytes = read(fd, events, sizeof(events));

        if (bytes < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return fail("read");
        }

        if (bytes == 0 || bytes % sizeof(events[0]) != 0) {
            puts("Invalid input event read length.");
            return 1;
        }

        size_t count = (size_t)bytes / sizeof(events[0]);

        for (size_t i = 0; i < count; ++i) {
            print_event(&events[i]);

            // SDL scancode 41 is Escape.
            if (events[i].type == 1 &&
                events[i].code == 41 &&
                events[i].value == 0) {
                running = 0;
            }
        }
    }

    if (close(fd) < 0) return fail("close");

    puts("Input test finished.");
    return 0;
}