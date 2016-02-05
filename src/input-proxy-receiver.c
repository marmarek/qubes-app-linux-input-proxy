#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include "protocol.h"
#include "common.h"

#define UINPUT_DEVICE "/dev/uinput"

struct options {
    char *name;
    int vendor;
    int product;
    struct input_proxy_device_caps caps;
};

void long_and(unsigned long *dst, unsigned long *src, size_t longs_count) {
    size_t i;

    for (i = 0; i < longs_count; i++)
        dst[i] &= src[i];
}
#define LONG_AND(dst, src) long_and(dst, src, sizeof(dst)/sizeof(dst[0]))

int long_test_bit(unsigned long *bitfield, int bit, size_t bitfield_size) {
    if (bit / BITS_PER_LONG >= bitfield_size)
        return 0;
    return (bitfield[bit / BITS_PER_LONG] &
                    (1UL<<(bit & (BITS_PER_LONG-1)))) != 0;
}
#define LONG_TEST_BIT(bitfield, bit) long_test_bit(bitfield, bit, \
        sizeof(bitfield)/sizeof(bitfield[0]))

void long_set_bit(unsigned long *bitfield, int bit, size_t bitfield_size) {
    if (bit / BITS_PER_LONG >= bitfield_size)
        return;

    bitfield[bit / BITS_PER_LONG] |= (1UL<<(bit & (BITS_PER_LONG-1)));
}
#define LONG_SET_BIT(bitfield, bit) long_set_bit(bitfield, bit, \
        sizeof(bitfield)/sizeof(bitfield[0]))


/* receive hello and device caps, then process according to opt->caps - allow
 * only those set there, then adjust opt->caps to have only really supported
 * capabilities by remote end
 *
 * returns: -1 on error, 0 on EOF, 1 on success
 */
int receive_and_validate_caps(struct options *opt) {
    struct input_proxy_device_caps untrusted_caps;
    struct input_proxy_hello untrusted_hello;
    int rc;

    rc = read_all(0, &untrusted_hello, sizeof(untrusted_hello));
    if (rc == 0)
        return 0;
    if (rc == -1) {
        perror("read hello");
        return -1;
    }

    if (untrusted_hello.version != INPUT_PROXY_PROTOCOL_VERSION) {
        fprintf(stderr, "Incompatible remote protocol version: %d\n",
                untrusted_hello.version);
        return -1;
    }

    /* TODO: handle smaller caps - just zero other fields */
    if (untrusted_hello.caps_size != sizeof(untrusted_caps)) {
        fprintf(stderr, "Incompatible device caps structure: %u != %lu\n",
                untrusted_hello.caps_size, sizeof(untrusted_caps));
        return -1;
    }
    rc = read_all(0, &untrusted_caps, sizeof(untrusted_caps));
    if (rc == 0)
        return 0;
    if (rc == -1) {
        perror("read caps");
        return -1;
    }

    LONG_AND(opt->caps.propbit, untrusted_caps.propbit);
    LONG_AND(opt->caps.evbit,   untrusted_caps.evbit);
#define APPLY_BITS(_evflag, _field) \
    if (LONG_TEST_BIT(opt->caps.evbit, _evflag)) \
        LONG_AND(opt->caps._field, untrusted_caps._field); \
    else \
        memset(opt->caps._field, 0, sizeof(opt->caps._field));

    APPLY_BITS(EV_KEY, keybit);
    APPLY_BITS(EV_REL, relbit);
    APPLY_BITS(EV_ABS, absbit);
    APPLY_BITS(EV_MSC, mscbit);
    APPLY_BITS(EV_LED, ledbit);
    APPLY_BITS(EV_SND, sndbit);
    APPLY_BITS(EV_FF,  ffbit);
    APPLY_BITS(EV_SW,  swbit);
#undef APPLY_BITS

    return 1;
}

int send_bits(int fd, int ioctl_num, unsigned long *bits, size_t bits_count) {
    size_t i;

    for (i = 0; i < bits_count; i++) {
        if (bits[i / BITS_PER_LONG] & (1UL<<(i & (BITS_PER_LONG-1)))) {
            if (ioctl(fd, ioctl_num, i) == -1) {
                perror("ioctl set bit");
                return -1;
            }
        }
    }
    return 0;
}


int register_device(struct options *opt, int fd) {
    struct uinput_user_dev uinput_dev = { 0 };
    int rc = 0;

    if (!rc)
        rc = send_bits(fd, UI_SET_EVBIT, opt->caps.evbit, EV_CNT);
    if (!rc && LONG_TEST_BIT(opt->caps.evbit, EV_KEY)) 
        rc = send_bits(fd, UI_SET_KEYBIT, opt->caps.keybit, KEY_CNT);
    if (!rc && LONG_TEST_BIT(opt->caps.evbit, EV_REL)) 
        rc = send_bits(fd, UI_SET_RELBIT, opt->caps.relbit, REL_CNT);
    if (!rc && LONG_TEST_BIT(opt->caps.evbit, EV_ABS)) 
        rc = send_bits(fd, UI_SET_ABSBIT, opt->caps.absbit, ABS_CNT);
    if (!rc && LONG_TEST_BIT(opt->caps.evbit, EV_MSC)) 
        rc = send_bits(fd, UI_SET_MSCBIT, opt->caps.mscbit, MSC_CNT);
    if (!rc && LONG_TEST_BIT(opt->caps.evbit, EV_LED)) 
        rc = send_bits(fd, UI_SET_LEDBIT, opt->caps.ledbit, LED_CNT);
    if (!rc && LONG_TEST_BIT(opt->caps.evbit, EV_SND)) 
        rc = send_bits(fd, UI_SET_SNDBIT, opt->caps.sndbit, SND_CNT);
    if (!rc && LONG_TEST_BIT(opt->caps.evbit, EV_FF)) 
        rc = send_bits(fd, UI_SET_FFBIT, opt->caps.ffbit, FF_CNT);
    if (!rc && LONG_TEST_BIT(opt->caps.evbit, EV_SW)) 
        rc = send_bits(fd, UI_SET_SWBIT, opt->caps.swbit, SW_CNT);
    if (rc == -1) {
        close(fd);
        return -1;
    }

    if (opt->name)
        strncpy(uinput_dev.name, opt->name, UINPUT_MAX_NAME_SIZE);
    uinput_dev.id.bustype = BUS_USB;
    uinput_dev.id.vendor = opt->vendor;
    uinput_dev.id.product = opt->product;
    uinput_dev.id.version = 1;
    /* TODO: support for uinput_dev.abs(min|max) */
    if (write_all(fd, &uinput_dev, sizeof(uinput_dev)) == -1) {
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) == -1) {
        perror("ioctl dev create");
        return -1;
    }
    return 0;
}

int validate_and_forward_event(struct options *opt, int src, int dst) {
    struct input_event untrusted_event;
    struct input_event ev = { 0 };
    int rc;

    rc = read_all(src, &untrusted_event, sizeof(untrusted_event));
    if (rc == 0)
        return 0;
    if (rc == -1) {
        perror("read event");
        return -1;
    }
    /* ignore untrusted_event.time */;

    if (LONG_TEST_BIT(opt->caps.evbit, untrusted_event.type) == 0)
        return 1; /* ignore unsupported/disabled events */
    ev.type = untrusted_event.type;
    switch (ev.type) {
        case EV_SYN:
            if (untrusted_event.code > SYN_MAX)
                return -1;
            ev.code = untrusted_event.code;
            ev.value = 0;
            break;
        case EV_KEY:
            if (LONG_TEST_BIT(opt->caps.keybit, untrusted_event.code) == 0)
                return 1; /* ignore unsupported/disabled key */
            ev.code = untrusted_event.code;
            /* XXX values: 0: release, 1: press, 2: repeat */
            ev.value = untrusted_event.value;
            break;
        case EV_REL:
            if (LONG_TEST_BIT(opt->caps.relbit, untrusted_event.code) == 0)
                return 1; /* ignore unsupported/disabled axis */
            ev.code = untrusted_event.code;
            ev.value = untrusted_event.value;
            break;
        case EV_ABS:
            if (LONG_TEST_BIT(opt->caps.absbit, untrusted_event.code) == 0)
                return 1; /* ignore unsupported/disabled axis */
            ev.code = untrusted_event.code;
            ev.value = untrusted_event.value;
            break;
        case EV_MSC:
            if (LONG_TEST_BIT(opt->caps.mscbit, untrusted_event.code) == 0)
                return 1; /* ignore unsupported/disabled */
            ev.code = untrusted_event.code;
            ev.value = untrusted_event.value;
            break;
        case EV_SW:
            if (LONG_TEST_BIT(opt->caps.swbit, untrusted_event.code) == 0)
                return 1; /* ignore unsupported/disabled */
            ev.code = untrusted_event.code;
            ev.value = untrusted_event.value;
            break;
        case EV_LED:
            if (LONG_TEST_BIT(opt->caps.ledbit, untrusted_event.code) == 0)
                return 1; /* ignore unsupported/disabled */
            ev.code = untrusted_event.code;
            ev.value = untrusted_event.value;
            break;
        case EV_SND:
            if (LONG_TEST_BIT(opt->caps.sndbit, untrusted_event.code) == 0)
                return 1; /* ignore unsupported/disabled */
            ev.code = untrusted_event.code;
            ev.value = untrusted_event.value;
            break;
        case EV_REP:
        case EV_FF:
        case EV_PWR:
        default:
            fprintf(stderr, "Unsupported event type %d\n", ev.type);
            return -1;
    }

    rc = write_all(dst, &ev, sizeof(ev));
    if (rc == -1)
        perror("write event");
    return rc;
}

int process_events(struct options *opt, int fd) {
    struct pollfd fds[] = {
        { .fd = 0,  .events = POLLIN, .revents = 0, },
        { .fd = fd, .events = POLLIN, .revents = 0, }
    };
    int rc = 0;

    while ((rc=poll(fds, 2, -1)) > 0) {
        if (fds[0].revents) {
            rc = validate_and_forward_event(opt, 0, fd);
            if (rc <= 0)
                return rc;
        }
        if (fds[1].revents) {
            rc = validate_and_forward_event(opt, fd, 1);
            if (rc <= 0)
                return rc;
        }
    }
    if (rc == -1) {
        perror("poll");
        return -1;
    }
    return 0;
}

void usage() {
    fprintf(stderr, "Usage: input-proxy-receiver [options...]\n");
    fprintf(stderr, "       --mouse, -m - allow remote device act as mouse\n");
    fprintf(stderr, "    --keyboard, -k - allow remote device act as keyboard\n");
    fprintf(stderr, "      --tablet, -t - allow remote device act as tablet\n");
    fprintf(stderr, "   --name=NAME, -n - set device name\n");
    fprintf(stderr, "   --vendor=ID,    - set device vendor ID (hex)\n");
    fprintf(stderr, "  --product=ID,    - set device product ID (hex)\n");
}

#define OPT_VENDOR  128
#define OPT_PRODUCT 129

int parse_options(struct options *opt, int argc, char **argv) {
    struct option opts[] = {
        { "mouse",     0, 0, 'm' },
        { "keyboard",  0, 0, 'k' },
        { "tablet",    0, 0, 't' },
        { "name",      1, 0, 'n' },
        { "vendor",    1, 0, OPT_VENDOR },
        { "product",   1, 0, OPT_PRODUCT },
        { 0 }
    };
    int o;

    memset(opt, 0, sizeof(*opt));
    opt->name = NULL;
    opt->vendor = 0xffff;
    opt->product = 0xffff;
    LONG_SET_BIT(opt->caps.evbit, EV_SYN);

    while ((o = getopt_long(argc, argv, "mktn:v:p:", opts, NULL)) != -1) {
        switch (o) {
            case 'm':
                LONG_SET_BIT(opt->caps.evbit, EV_REL);
                LONG_SET_BIT(opt->caps.evbit, EV_KEY);
                /* TODO: some configuration for that */
                memset(opt->caps.relbit, 0xff, sizeof(opt->caps.relbit));
                LONG_SET_BIT(opt->caps.keybit, BTN_LEFT);
                LONG_SET_BIT(opt->caps.keybit, BTN_RIGHT);
                LONG_SET_BIT(opt->caps.keybit, BTN_MIDDLE);
                break;
            case 'k':
                LONG_SET_BIT(opt->caps.evbit, EV_KEY);
                LONG_SET_BIT(opt->caps.evbit, EV_LED);
                /* TODO: some configuration for that */
                memset(opt->caps.keybit, 0xff, sizeof(opt->caps.keybit));
                memset(opt->caps.ledbit, 0xff, sizeof(opt->caps.ledbit));
                break;
            case 't':
                LONG_SET_BIT(opt->caps.evbit, EV_ABS);
                /* TODO: absmax */
                break;
            case 'n':
                opt->name = optarg;
                break;
            case OPT_VENDOR:
                opt->vendor = strtoul(optarg, NULL, 16);
                break;
            case OPT_PRODUCT:
                opt->product = strtoul(optarg, NULL, 16);
                break;
            default:
                usage();
                return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    struct options opt;
    int fd;
    int rc;

    rc = parse_options(&opt, argc, argv);
    if (rc == -1)
        return 1;

    rc = receive_and_validate_caps(&opt);
    if (rc <= 0)
        return rc == -1;

    fd = open(UINPUT_DEVICE, O_RDWR);
    if (fd == -1) {
        perror("open " UINPUT_DEVICE);
        return 1;
    }

    rc = register_device(&opt, fd);
    if (rc == -1) {
        close(fd);
        return 1;
    }

    rc = process_events(&opt, fd);
    if (rc == -1) {
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
