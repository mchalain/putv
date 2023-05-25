#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <linux/uinput.h>

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

void emit(int fd, int type, int code, int val)
{
    int ret = 0;
    struct input_event ie;

    gettimeofday(&ie.time,0);
    ie.type = type;
    ie.code = code;
    ie.value = val;

    ret = write(fd, &ie, sizeof(ie));
    if (ret != sizeof(ie))
	err("write error");
}

void emit_key(int fd, int key)
{
   /* Key press, report the event, send key release, and report again */
   emit(fd, EV_KEY, key, 1);
   emit(fd, EV_SYN, SYN_REPORT, 0);
   emit(fd, EV_KEY, key, 0);
   emit(fd, EV_SYN, SYN_REPORT, 0);
}

int main(void)
{
    struct uinput_setup usetup;
    int ret;
    struct termios state;
    int fd;
    int run = 1;

    ret = tcgetattr(STDIN_FILENO, &state);
    if (ret)
    {
	    err("input: Cannot get tty state");
    }
    else
    {
	    state.c_lflag &= ~ICANON;
	    state.c_cc[VMIN] = 1;
	    ret = tcsetattr(STDIN_FILENO, TCSANOW, &state);
	    if (ret)
		    err("input: Cannot set tty state");
    }

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
    {
       err("/dev/uinput openning error");
       return 1;
    }
    /*
    * The ioctls below will enable the device that is about to be
    * created, to pass key events, in this case the space key.
    */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, KEY_PLAY);
    ioctl(fd, UI_SET_KEYBIT, KEY_PAUSE);
    ioctl(fd, UI_SET_KEYBIT, KEY_STOP);
    ioctl(fd, UI_SET_KEYBIT, KEY_NEXT);

    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234; /* sample vendor */
    usetup.id.product = 0x5678; /* sample product */
    strcpy(usetup.name, "Example device");

    ret = ioctl(fd, UI_DEV_SETUP, &usetup);
    if (ret != 0)
	err("dev setup error");
    ret = ioctl(fd, UI_DEV_CREATE);
    if (ret != 0)
	err("dev create error");

    /*
    * On UI_DEV_CREATE the kernel will create the device node for this
    * device. We are inserting a pause here so that userspace has time
    * to detect, initialize the new device, and can start listening to
    * the event, otherwise it will not notice the event we are about
    * to send. This pause is only needed in our example code!
    */
    sleep(1);
    while (run)
    {
	char string[256] = {0};
	ret = read(STDIN_FILENO, string, 255);
	for (int i = 0; i < ret; i++)
	{
	    if (ret - i > 1 && !strcmp(string,"play"))
	    {
		emit_key(fd, KEY_PLAY);
		i += 4;
	    }
	    else if (ret - i > 1 && !strcmp(string, "pause"))
	    {
		emit_key(fd, KEY_PAUSE);
		i += 5;
	    }
	    else if (ret - i > 1 && !strcmp(string, "stop"))
	    {
		emit_key(fd, KEY_STOP);
		i += 4;
	    }
	    else if (ret - i > 1 && !strcmp(string, "next"))
	    {
		emit_key(fd, KEY_NEXT);
		i += 4;
	    }
	    else if (ret - i > 1 && !strcmp(string, "quit"))
	    {
		run = 0;
	    }
	    else if (string[0] == 'p')
		emit_key(fd, KEY_PLAY);
	    else if (string[0] == 's')
		emit_key(fd, KEY_STOP);
	    else if (string[0] == 'n')
		emit_key(fd, KEY_NEXT);
	    else if (string[0] == 'q')
		run = 0;
	    else
		warn("unkown command");
	}
    }
    /*
    * Give userspace some time to read the events before we destroy the
    * device with UI_DEV_DESTOY.
    */
    sleep(1);

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);

    return 0;
}
