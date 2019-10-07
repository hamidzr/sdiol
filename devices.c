#include "devices.h"
#include "config.h"

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

const char *grab_by_name[] = {"keyboard", "ergodox"};

int open_output() {
  int i;
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    return -1;
  }

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_EVBIT, EV_SYN);
  for (i = 0; i < 256; i++)
    ioctl(fd, UI_SET_KEYBIT, i);

  struct uinput_user_dev uidev;

  memset(&uidev, 0, sizeof(uidev));

  snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "keytap");
  uidev.id.bustype = BUS_USB;
  uidev.id.vendor = 0x1111;
  uidev.id.product = 0x0001;
  uidev.id.version = 1;

  write(fd, &uidev, sizeof(uidev));
  ioctl(fd, UI_DEV_CREATE);

  return fd;
}


// check the linked list of grabs and return a root keymap if we should grab
key_action_t *check_grabs(grab_t *grabs, const char *name){
    for(grab_t *grab = grabs; grab != NULL; grab = grab->next){
        int ret = regexec(&grab->regex, name, 0, NULL, 0);
        if(ret != REG_NOMATCH){
            return grab->ignore ? NULL : &grab->map;
        }
    }
    return NULL;
}

// return true if we decided to grab the device
static bool open_input(char *dev, grab_t *grabs, int *fd_out,
        key_action_t **map_out){
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", dev, strerror(errno));
        return false;
    }

    char buf[256];
    ioctl(fd, EVIOCGNAME(sizeof(buf)), buf);
    key_action_t *map;
    if((map = check_grabs(grabs, buf)) != NULL){
        int ret = ioctl(fd, EVIOCGRAB, 1);
        if (ret < 0) {
            fprintf(stderr, "%s: %s\n", dev, strerror(errno));
            close(fd);
            return false;
        } else {
            *fd_out = fd;
            *map_out = map;
            return true;
        }
    }
    return false;
}

void open_inputs(keyboard_t *kbs, int *n_kbs, grab_t *grabs, send_t send,
        void *send_data){
    *n_kbs = 0;

    char dev[512];

    DIR *d = opendir("/dev/input");
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name != strstr(ent->d_name, "event"))
            continue;

        snprintf(dev, sizeof(dev), "/dev/input/%s", ent->d_name);

        if(*n_kbs < MAX_KBS){
            int fd;
            key_action_t *map;
            if(!open_input(dev, grabs, &fd, &map))
                continue;

            keyboard_t kb;
            kb.fd = fd;
            printf("resolver has root_keymap %p\n", map);
            resolver_init(&kb.resolv, map, send, send_data);

            kbs[(*n_kbs)++] = kb;
        }
    }
    closedir(d);
}

void handle_inotify_events(int inot, keyboard_t *kbs, int* n_kbs,
        grab_t *grabs, send_t send, void *send_data){
    // most of this section is straight from `man 7 inotify`
    char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *event;

    ssize_t len = read(inot, buf, sizeof(buf));
    if(len == -1 && errno != EAGAIN){
        perror("read");
        exit(3);
    }

    // EAGAIN means we are out of events for now.
    if(len <= 0) return;

    // read all the events we just got
    for(char *ptr = buf; ptr < buf + len;
            ptr += sizeof(struct inotify_event) + event->len){
        event = (const struct inotify_event *)ptr;

        if(*n_kbs < MAX_KBS){
            char dev[512];
            snprintf(dev, sizeof(dev), "/dev/input/%s", event->name);
            int fd;
            key_action_t *map;
            if(!open_input(dev, grabs, &fd, &map))
                continue;

            keyboard_t kb;
            kb.fd = fd;
            resolver_init(&kb.resolv, map, send, send_data);

            kbs[(*n_kbs)++] = kb;
        }
    }
}

int open_inotify() {
  int inot = inotify_init();
  inotify_add_watch(inot, "/dev/input", IN_CREATE);

  return inot;
}
