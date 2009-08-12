#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/limits.h>
#include <sys/poll.h>
#include <linux/input.h>
#include <errno.h>
#include <cutils/log.h>

static struct pollfd *ufds;
static char **device_names;
static int nfds;

static int open_device(const char *device)
{
    int version;
    int fd;
    struct pollfd *new_ufds;
    char **new_device_names;
    char name[80];
    char location[80];
    char idstr[80];
    struct input_id id;

    fd = open(device, O_RDWR);
    if(fd < 0) {
        return -1;
    }
    /* XXX: unused { */
    if(ioctl(fd, EVIOCGVERSION, &version)) {
        return -1;
    }
    if(ioctl(fd, EVIOCGID, &id)) {
        return -1;
    }
    /* XXX: unused } */
    name[sizeof(name) - 1] = '\0';
    location[sizeof(location) - 1] = '\0';
    idstr[sizeof(idstr) - 1] = '\0';
    if(ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1) {
        //fprintf(stderr, "could not get device name for %s, %s\n", device, strerror(errno));
        name[0] = '\0';
    }
    if(ioctl(fd, EVIOCGPHYS(sizeof(location) - 1), &location) < 1) {
        //fprintf(stderr, "could not get location for %s, %s\n", device, strerror(errno));
        location[0] = '\0';
    }
    if(ioctl(fd, EVIOCGUNIQ(sizeof(idstr) - 1), &idstr) < 1) {
        //fprintf(stderr, "could not get idstring for %s, %s\n", device, strerror(errno));
        idstr[0] = '\0';
    }

    new_ufds = realloc(ufds, sizeof(ufds[0]) * (nfds + 1));
    if(new_ufds == NULL) {
        fprintf(stderr, "open_device(): failed to allocate memory for ufds\n");
        return -1;
    }
    ufds = new_ufds;
    new_device_names = realloc(device_names, sizeof(device_names[0]) * (nfds + 1));
    if(new_device_names == NULL) {
        fprintf(stderr, "open_device(): failed to allocate memory for device names\n");
        return -1;
    }
    device_names = new_device_names;
    ufds[nfds].fd = fd;
    ufds[nfds].events = POLLIN;
    device_names[nfds] = strdup(device);
    nfds++;

    return 0;
}

int close_device(const char *device)
{
    int i;
    for(i = 1; i < nfds; i++) {
        if(strcmp(device_names[i], device) == 0) {
            int count = nfds - i - 1;
            free(device_names[i]);
            memmove(device_names + i, device_names + i + 1,
	        sizeof(device_names[0]) * count);
            memmove(ufds + i, ufds + i + 1, sizeof(ufds[0]) * count);
            nfds--;
            return 0;
        }
    }
    return -1;
}

static int read_notify(const char *dirname, int nfd,
    int allow_skip)
{
    int res;
    char devname[PATH_MAX];
    char event_buf[512];
    int event_size;
    int event_pos = 0;
    struct inotify_event *event;

    res = read(nfd, event_buf, sizeof(event_buf));
    if(res < (int)sizeof(*event)) {
        if(errno == EINTR)
            return 0;
        fprintf(stderr, "could not get event, %s\n", strerror(errno));
        return 1;
    }
    //printf("got %d bytes of event information\n", res);

    while(res >= (int)sizeof(*event)) {
        event = (struct inotify_event *)(event_buf + event_pos);
        //printf("%d: %08x \"%s\"\n", event->wd, event->mask, event->len ? event->name : "");
        if(event->len > 0) {
	    int result = snprintf(devname, sizeof(devname), "%s/%s", dirname,
                             event->name);
    
            if (result >= (int)sizeof(devname)) {
	        fprintf(stderr, "devname buffer size %zu was not big enough "
		    "to store %d bytes\n", sizeof(devname), result);

                if (allow_skip == 0)
		    return 1;
	    }
	    else {
	    
                if(event->mask & IN_CREATE) {
                    open_device(devname);
                }
                else {
                    close_device(devname);
                }
	    }
        }
        event_size = sizeof(*event) + event->len;
        res -= event_size;
        event_pos += event_size;
    }
    return 0;
}

static int scan_dir(const char *dirname)
{
    char devname[PATH_MAX];
    DIR *dir;
    struct dirent *de;
    dir = opendir(dirname);
    if(dir == NULL)
        return -1;
    while((de = readdir(dir))) {
        if(de->d_name[0] == '.' &&
           (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
	int result = snprintf(devname, sizeof(devname), "%s/%s", dirname,
                        de->d_name);
        if (result < (int)sizeof(devname))
            open_device(devname);
	else
	    fprintf(stderr, "scan_dir(): filename '%s' is too big, skipping\n",
	        de->d_name);
    }
    closedir(dir);
    return 0;
}

int init_getevent()
{
    int res;
    const char *device_path = "/dev/input";

    nfds = 1;
    ufds = calloc(1, sizeof(ufds[0]));
    ufds[0].fd = inotify_init();
    ufds[0].events = POLLIN;

    res = inotify_add_watch(ufds[0].fd, device_path, IN_DELETE | IN_CREATE);
    if(res < 0) {
        return 1;
    }
    res = scan_dir(device_path);
    if(res < 0) {
        return 1;
    }
    return 0;
}

void uninit_getevent()
{
    int i;
    for(i = 0; i < nfds; i++) {
        close(ufds[i].fd);
    }
    free(ufds);
    ufds = 0;
    nfds = 0;
}

int get_event(struct input_event *event, int timeout)
{
    int res;
    int i;
    int pollres;
    const char *device_path = "/dev/input";
    while(1) {
        pollres = poll(ufds, nfds, timeout);
        if (pollres == 0) {
            return 1;
        }
        if(ufds[0].revents & POLLIN) {
            read_notify(device_path, ufds[0].fd, 0);
        }
        for(i = 1; i < nfds; i++) {
            if(ufds[i].revents) {
                if(ufds[i].revents & POLLIN) {
                    res = read(ufds[i].fd, event, sizeof(*event));
                    if(res < (int)sizeof(event)) {
                        fprintf(stderr, "get_event(): could not read event\n");
                        return -1;
                    }
                    return 0;
                }
            }
        }
    }
    return 0;
}
