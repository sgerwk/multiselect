/*
 * multiselect.c
 *
 * provide multiple selections in X11
 */

/*
 * todo:
 * - optionlly, send selection one-by-one by XTestFakeKeyEvent()
 *   option: -f #
 *   effect:	#click and #down are sent as button and key events
 *		#n = click + n keydown
 * - multiselect only makes a single request for the selection when adding one;
 *   the result may be only an initial part of the selection if it is long
 */

/*
 * state variables
 * 	pending		a program requests the selection, which is not sent yet
 * 	showing		the selection menu is on the screen
 */

/*
 * shorttime
 *
 * ideally, checking how much time passed should not be necessary: a request
 * for the selection arrives, the window is opened, the users chooses a string
 * which is answered back to the requestor
 *
 * this mechanism does not work well when the user decides not to paste any of
 * the strings; the selection transmission protocol of ICCCM only allows
 * refusing the selection as an alternative to sending it; but refusal may make
 * the other program request the selection again with a different conversion;
 * xterm does this, for example
 *
 * other clients may request the selection twice for no reason, which usually
 * causes no problem since the selection does not change if not by a user
 * action; opera does this, for example
 *
 * in both cases, a second request arrives right after the user has chosen a
 * string or none of them, causing the window to be shown again waiting for
 * another choice from the user
 *
 * the solution is to store the time of the last request (except those for
 * TARGETS, which are served immediately anyway); if another request arrives in
 * a very short time (1/100 of a second), it is served in the same way: with
 * the same string or with a refusal as done for the previous request
 */

/*
 * the cut buffer
 *
 * the user may decide not to paste any of the stored strings by pressing an
 * invalid key; this causes the request for the selection to be refused, as
 * well as every following one arrived in a short time; when this happens, the
 * other client may decide the use the cut buffer
 *
 * this is wrong because the user has decided not to paste anything, and
 * certainly is not expecting a string not among the selections; this is why
 * multiselect deletes the cut buffer at startup
 */

/*
 * sending a middle button click
 *
 * some programs ignore selection notify events if they arrive past a certain
 * time after they requested the selection; sending the selection to them has
 * no effect
 *
 * the solution is to simulate a middle button click when the user chooses a
 * string; this typically causes the program that requested the selection to do
 * that again; this request is served immediately; all other requests are
 * refused; the position of the pointer has to be saved and restored, as
 * otherwise the click would be in a different position
 *
 * since middle button clicks do not mandate pasting, another mechanism can be
 * employed: when the user chooses a string, it is sent to the requestor
 * immediately; this behaviour is activated when the boolean variable 'click'
 * is false (option -p)
 */

/*
 * firefox
 *
 * the problem with firefox is that due to bad programming, it asks for the
 * selection in a specific do/while loop with a timeout kClipboardTimeout of
 * half a second, unchangeable by configuration options; this means that the
 * user has only half a second to choose the string to paste
 *
 * the hack that is currently implemented is to detect firefox by a specific
 * requests it done for a conversion to type "text/x-moz-text-internal" after
 * its timeout expires; to facilitate the user, the next time a selection is
 * requested, the previous string chosen is sent again
 *
 * this is only done when the selection is sent immediately (option -p)
 */

/*
 * the flash window
 *
 * when the user adds a selection by ctrl-shift-z or by F2 (if enabled by -k),
 * the selections are briefly shown to confirm that the addition succeded
 *
 * this is done by a window that is not the multiselect window because of its
 * different treatment of events: only Expose events matter, and they cause the
 * window to be redrawn and closed after a short time
 *
 * ideally, the window should only be redrawn on Expose events, while closure
 * be controlled by a timeout; just flushing the X queue, waiting and closing
 * the window in response to Expose events seems to work anyway; a fallback
 * based on the elapsed time ensures that the window is eventually closed even
 * if no Expose event is received
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

/*
 * fake event to open the selection window
 */
#define ShowWindow LASTEvent

/*
 * maximum number of strings
 */
#define MAXNUM 20

/*
 * font
 */
#define FONT "-*-*-medium-r-*-*-18-*-*-*-m-*-iso10646-1"
#define WMNAME "multiselect"
#define WMNAMEDAEMON "multiselectd"

/*
 * check the existence of a window with a certain name
 */
Bool WindowNameExists(Display *d, Window root, char *name) {
	Window rootout, parent, *children;
	unsigned int nchildren, i;
	char *p;

	XQueryTree(d, root, &rootout, &parent, &children, &nchildren);
	for (i = 0; i < nchildren; i++) {
		if (XFetchName(d, children[i], &p) == 0)
			continue;
		if (! strcmp(name, p)) {
			XFree(children);
			return True;
		}
	}

	XFree(children);
	return False;
}

/*
 * grab a key
 */
Bool GrabKey(Display *d, Window r, KeySym keysym, unsigned int modifiers) {
	Bool res;
	res = XGrabKey(d, XKeysymToKeycode(d, keysym), modifiers, r, False,
			GrabModeAsync, GrabModeAsync);
	if (res == True)
		printf("grabbed key %ld\n", keysym);
	else
		printf("grabbing of key %ld failed\n", keysym);
	return res;
}

/*
 * check whether a short time passed since the last call
 */
Bool ShortTime(struct timeval *last, int interval, Bool store) {
	struct timeval now;
	Bool ret = True;

	gettimeofday(&now, NULL);

	if (now.tv_sec >= last->tv_sec + 2)
		last->tv_sec = now.tv_sec - 2;
	ret = now.tv_usec + 1000000 * (now.tv_sec - last->tv_sec)
	      <= last->tv_usec + interval;

	if (store) {
		printf("shorttime: %s ", ret ? "True" : "False");
		printf("(%ld,%ld -> ", last->tv_sec % 60, last->tv_usec);
		printf("%ld,%ld)\n", now.tv_sec % 60, now.tv_usec);
		*last = now;
	}
	return ret;
}

/*
 * resize the window to fit the current number of strings
 */
void ResizeWindow(Display *d, Window w, XFontStruct *fs, int num) {
	int width, height;
	width = 400;
	height = (fs->ascent + fs->descent) * (num + 1);
	XResizeWindow(d, w, width, height);
}

/*
 * position of the pointer
 */
void PointerPosition(Display *d, Window r, int *x, int *y) {
	Window child;
	int wx, wy;
	unsigned int mask;

	XQueryPointer(d, r, &r, &child, x, y, &wx, &wy, &mask);
}

/*
 * place the window at cursor
 */
void WindowAtPointer(Display *d, Window w) {
	int x, y;
	unsigned int width, height, border, depth, rwidth, rheight, rborder;
	Window root, r;

	XGetGeometry(d, w, &root, &x, &y, &width, &height, &border, &depth);
	XGetGeometry(d, root, &r, &x, &y, &rwidth, &rheight, &rborder, &depth);

	PointerPosition(d, root, &x, &y);
	x =  x - (int) width / 2;
	if (x < 0)
		x = border;
	if (x + width >= rwidth)
		x = rwidth - width - 2 * border;
	if (y + 10 + height + 2 * border < rheight)
		y = y + 10;
	else if (y - 10 - (int) height > 10)
		y = y - 10 - (int) height;
	else
		y = rheight - 10 - (int) height;

	XMoveWindow(d, w, x, y);
	printf("window moved at x=%d y=%d\n", x, y);
}

/*
 * print an atom name
 */
void PrintAtomName(Display *d, Atom a) {
	char *name;

	name = XGetAtomName(d, a);
	printf("atom %s", name);
	XFree(name);
}

/*
 * get a timestamp for "now" (see ICCCM)
 */
Time GetTimestampForNow(Display *d, Window w) {
	XEvent e;

	XChangeProperty(d, w, XA_CURSOR, XA_STRING, 8, PropModeAppend, NULL, 0);
	XWindowEvent(d, w, PropertyChangeMask, &e);
	return e.xproperty.time;
}

/*
 * request the selection
 */
Bool RequestPrimarySelection(Display *d, Window w) {
	if (XGetSelectionOwner(d, XA_PRIMARY) == None) {
		printf("owner is none\n");
		return False;
	}
	if (XGetSelectionOwner(d, XA_PRIMARY) == w) {
		printf("owner is self\n");
		return False;
	}
	XConvertSelection(d, XA_PRIMARY, XA_STRING, XA_PRIMARY, w, CurrentTime);
	return True;
}

/*
 * acquire ownership of the primary selection
 */
Bool AcquirePrimarySelection(Display *d, Window root, Window w, Time *t) {
	Window o;

	XSetSelectionOwner(d, XA_PRIMARY, w, CurrentTime);
	o = XGetSelectionOwner(d, XA_PRIMARY);
	if (o == w)
		printf("aquired selection ownership\n");
	else {
		printf("cannot get selection ownership\n");
		return True;
	}
	if (t != NULL)
		*t = GetTimestampForNow(d, w);
	XDeleteProperty(d, root, XInternAtom(d, "CUT_BUFFER0", True));
	return False;
}

/*
 * refuse to send selection
 */
void RefuseSelection(Display *d, XSelectionRequestEvent *re) {
	XEvent ne;

	printf("refusing to send selection\n");

	ne.type = SelectionNotify;
	ne.xselection.requestor = re->requestor;
	ne.xselection.selection = re->selection;
	ne.xselection.target = re->target;
	ne.xselection.property = None;
	ne.xselection.time = re->time;

	XSendEvent(d, re->requestor, True, NoEventMask, &ne);
}

/*
 * check whether target of selection is supported
 */
Bool UnsupportedSelection(Display *d, Atom type, int stringonly) {
	if (type == XA_STRING)
		return False;
	if (type == XInternAtom(d, "TARGETS", False))
		return False;
	if (! stringonly && type == XInternAtom(d, "UTF8_STRING", False))
		return False;
	return True;
}

/*
 * send the selection to answer a selection request event
 *
 * - it the requested target is not a string (or utf8), do not send it
 * - if the property is none, use the target as the property
 * - check the timestamp: send the selection only if the timestamp is after the
 *   selection ownership assignment (note: CurrentTime may be implemented as 0)
 * - change the property of the requestor
 * - notify the requestor by a PropertyNotify event
 */
Bool SendSelection(Display *d, Time t, XSelectionRequestEvent *re,
		char *chars, int nchars, int stringonly) {
	XEvent ne;
	Atom property;
	int targetlen;
	Atom targetlist[2];

				/* check type of selection requested */

	if (UnsupportedSelection(d, re->target, stringonly)) {
		printf("request for an unsupported type\n");
		RefuseSelection(d, re);
		return True;
	}

				/* check property (obsolete clients) */

	if (re->property != None)
		property = re->property;
	else {
		printf("note: property is None\n");
		property = re->target;
	}

				/* request precedes time of ownership */

	if (re->time < t && re->time != CurrentTime) {
		printf("request precedes selection ownership: ");
		printf("%ld < %ld\n", re->time, t);
		RefuseSelection(d, re);
		return True;
	}

				/* store the selection or the targets */

	if (re->target == XInternAtom(d, "TARGETS", True)) {
		targetlen = 0;
		targetlist[targetlen++] = XInternAtom(d, "STRING", True);
		if (! stringonly)
			targetlist[targetlen++] =
				XInternAtom(d, "UTF8_STRING", True);
		printf("storing selection TARGETS\n");
		XChangeProperty(d, re->requestor, re->property, // re->target,
			XInternAtom(d, "ATOM", True), 32,
			PropModeReplace,
			(unsigned char *) &targetlist, targetlen);
	}
	else {
		printf("storing selection: %s\n", chars);
		XChangeProperty(d, re->requestor, re->property, re->target, 8,
			PropModeReplace,
			(unsigned char *) chars, nchars);
	}

				/* send notification */

	ne.type = SelectionNotify;
	ne.xselection.requestor = re->requestor;
	ne.xselection.selection = re->selection;
	ne.xselection.target = re->target;
	ne.xselection.property = property;
	ne.xselection.time = re->time;

	XSendEvent(d, re->requestor, True, NoEventMask, &ne);

	printf("selection sent and notified\n");

	return False;
}

/*
 * answer a request for the selection
 */
Bool AnswerSelection(Display *d, Time t, XSelectionRequestEvent *request,
		char **buffers, char separator, int key, int stringonly) {
	char *selection, *start;

	if (key == -1) {
		RefuseSelection(d, request);
		return False;
	}

	if (separator == '\0')
		selection = buffers[key];
	else {
		start = strchr(buffers[key], separator);
		selection = start ? start + 1 : buffers[key];
	}
	return SendSelection(d, t, request,
		selection, strlen(selection), stringonly);
}

/*
 * retrieve the selection
 */
char *GetSelection(Display *d, Window w, Atom selection, Atom target) {
	Bool res;
	int format;
	unsigned long i, nitems, after;
	unsigned char *string;
	Atom actualtype;
	char *r;

	res = XGetWindowProperty(d, w, selection, 0, 200, True, target,
		&actualtype, &format, &nitems, &after, &string);
	if (res != Success)
		return NULL;
	if (actualtype != XA_STRING)
		return NULL;

	printf("bytes left: %lu\n", after);
	printf("selection received: ");
	for (i = 0; i < nitems; i++)
		printf("%c", string[i]);
	printf("\n");

	r = strdup((char *) string);
	XFree(string);
	return r;
}

/*
 * index of a key
 */
int keyindex(int k) {
	if (k >= '1' && k <= '9')
		return k - '1';
	if (k >= 'a' && k <= 'z')
		return k - 'a' + 9;
	return -1;
}
char _keylabel[20];
char *keylabel(int k) {
	if (k < 10)
		sprintf(_keylabel, "%d", k);
	else
		sprintf(_keylabel, "%c", k + 'a' - 10);
	return _keylabel;
}

/*
 * window parameters
 */
struct WindowParameters {
	GC g;
	XFontStruct *fs;
	int black;
	int white;
};

/*
 * draw the window
 */
void draw(Display *d, Window w, struct WindowParameters *wp,
		char *buffers[], int n, int selected, char *message) {
	Window r;
	int x, y;
	unsigned int width, height, bw, depth, twidth;
	int lpos, interline;
	int i;
	char num[12], help[] = "multiselect";

	XClearWindow(d, w);
	XGetGeometry(d, w, &r, &x, &y, &width, &height, &bw, &depth);

	interline = wp->fs->ascent + wp->fs->descent;
	lpos = wp->fs->ascent;

	for (i = -1; i < n; i++) {
		XSetBackground(d, wp->g, wp->white);
		XSetForeground(d, wp->g, wp->black);
		if (i != selected && i != -1)
			XDrawLine(d, w, wp->g,
				0, lpos + wp->fs->descent,
				width, lpos + wp->fs->descent);
		else {
			XFillRectangle(d, w, wp->g,
				0, lpos - wp->fs->ascent,
				width, interline);
			XSetBackground(d, wp->g, wp->black);
			XSetForeground(d, wp->g, wp->white);
		}
		if (i == -1) {
			XDrawString(d, w, wp->g, 0, lpos,
				help, MIN(sizeof(help) - 1, 100));
			XFillRectangle(d, w, wp->g,
				width - interline * 2 - 3,
				lpos - wp->fs->ascent + 1,
				interline,
				lpos + wp->fs->descent - 3);
			XFillRectangle(d, w, wp->g,
				width - interline - 1,
				lpos - wp->fs->ascent + 1,
				interline,
				lpos + wp->fs->descent - 3);
			XSetForeground(d, wp->g, wp->black);
			XSetLineAttributes(d, wp->g, 5,
				LineSolid, CapRound, JoinMiter);
			x = width - interline - 6 - 2;
			y = lpos;
			XDrawLine(d, w, wp->g,
				x - (interline - 8) / 2, y,
				x, y - wp->fs->ascent + 5);
			XDrawLine(d, w, wp->g,
				x - (interline - 8) / 2, y,
				x - interline + 8, y - wp->fs->ascent + 5);
			x = width - 6;
			y = lpos;
			XDrawLine(d, w, wp->g,
				x - interline + 8, y,
				x, y - wp->fs->ascent + 5);
			XDrawLine(d, w, wp->g,
				x - interline + 8, y - wp->fs->ascent + 5,
				x, y);
			XSetLineAttributes(d, wp->g, 1,
				LineSolid, CapButt, JoinMiter);
		}
		else {
			if (i + 1 < 10)
				sprintf(num, "%d ", i + 1);
			else
				sprintf(num, "%c ", i + 'a' - 9);
			XDrawString(d, w, wp->g, 0, lpos, num, strlen(num));
			twidth = XTextWidth(wp->fs, num, strlen(num));
			XDrawString(d, w, wp->g, twidth, lpos,
				buffers[i], MIN(strlen(buffers[i]), 100));
		}
		lpos += interline;
	}

	if (message == NULL)
		return;

	twidth = XTextWidth(wp->fs, message, strlen(message));
	XFillRectangle(d, w, wp->g,
		(width - twidth) / 2 - 20, height / 2,
		twidth + 40, interline);
	XSetForeground(d, wp->g, wp->white);
	XDrawString(d, w, wp->g,
		(width - twidth) / 2, interline + height / 2 - 8,
		message, MIN(strlen(message), 100));
}

/*
 * main
 */
int main(int argc, char *argv[]) {
	Display *d;
	Screen *s;
	Window r, w, f;
	XColor sc;
	char *font = FONT;
	struct WindowParameters wp, fp;
	XSetWindowAttributes swa;

	Time t;
	struct timeval last, flashtime;
	int interval = 80000, hide;
	int starthide = 800000, changehide = 500000, messagehide = 800000;
	char *message = NULL, *selectmessage = "select a string first";
	Bool exitnext, stayinloop;
	Bool pending, showing, firefox, chosen, changed, keep;
	XEvent e;
	XSelectionRequestEvent *re, request;
	KeySym k;
	Window prev, pprev;
	XWindowAttributes wa;
	int il;
	int selected;
	int ret, pret;
	int key;
	int x, y, xb, yb;
	unsigned int dm;

	int opt;
	Bool daemon = False, daemonother, continuous = False;
	Bool immediate = False;
	Bool click = True;
	Bool f1 = False, f2 = False, f5 = False, force = False;
	Bool usage = False;
	char **buffers, separator, *terminator;
	int a, num;

	(void) dm;

				/* parse arguments */

	while (-1 != (opt = getopt(argc, argv, "dk:fcit:ph"))) {
		switch (opt) {
		case 'd':
			daemon = True;
			break;
		case 'k':
			if (! strcmp(optarg, "F1"))
				f1 = True;
			else if (! strcmp(optarg, "F2"))
				f2 = True;
			else if (! strcmp(optarg, "F5"))
				f5 = True;
			else {
				printf("only F1, F2 and F5 ");
				printf("currently supported\n");
				exit(EXIT_FAILURE);
			}
			daemon = True;
			break;
		case 'f':
			force = True;
			break;
		case 'c':
			continuous = True;
			daemon = True;
			break;
		case 'i':
			immediate = True;
			break;
		case 'p':
			click = False;
			break;
		case 't':
			separator = optarg[0];
			break;
		case 'h':
			usage = True;
			break;
		default:
			exit(EXIT_FAILURE);
		}
	}
	argc -= optind - 1;
	argv += optind - 1;
	if (argc - 1 == 1 && ! strcmp(argv[1], "-")) {
		printf("reading selections from stdin\n");
		buffers = malloc(MAXNUM * sizeof(char *));
		for (num = 0; num < MAXNUM; num++) {
			buffers[num] = malloc(500 * sizeof(char));
			if (NULL == fgets(buffers[num], 500, stdin)) {
				free(buffers[num]);
				break;
			}
			terminator = strrchr(buffers[num], '\n');
			if (terminator)
				*terminator = '\0';
		}
	}
	else {
		num = MIN(argc - 1, MAXNUM);
		buffers = malloc(MAXNUM * sizeof(char *));
		for (a = 0; a < num; a++)
			buffers[a] = strdup(argv[a + 1]);
	}

				/* usage */

	if (usage) {
		printf("multiple selection chooser\n");
		printf("usage:\n");
		printf("\tmultiselect [options] (-|string...)\n");
		printf("\toptions:\n");
		printf("\t\t-d\tkeep running to add new strings\n");
		printf("\t\t-k Fx\tenable a function key\n");
		printf("\t\t-c\tadd selected string immediately\n");
		printf("\t\t-i\tpaste immediately on up and down\n");
		printf("\t\t-t sep\tlabel separator\n");
		printf("\t\t-p\tpaste mode\n");
		printf("\t\t-h\tthis help\n");
		return EXIT_SUCCESS;
	}

				/* open display */

	d = XOpenDisplay(NULL);
	if (d == NULL) {
		printf("Cannot open display %s\n", XDisplayName(NULL));
		exit(EXIT_FAILURE);
	}
	s = DefaultScreenOfDisplay(d);
	r = DefaultRootWindow(d);
	printf("root window: 0x%lx\n", r);

				/* run or not, daemon or not */

	daemonother = WindowNameExists(d, r, WMNAMEDAEMON);
	if (WindowNameExists(d, r, WMNAME) || (daemon && daemonother)) {
		printf("%s already running\n", WMNAME);
		XCloseDisplay(d);
		exit(EXIT_FAILURE);
	}
	if (f1)
		GrabKey(d, r, XK_F1, 0);
	if (daemon)
		GrabKey(d, r, XK_z, ControlMask | ShiftMask);
	if (f2)
		GrabKey(d, r, XK_F2, 0);
	if (f5)
		GrabKey(d, r, XK_F5, 0);

				/* create the window and select input */

	swa.background_pixel = WhitePixelOfScreen(s);
	swa.override_redirect = True;
	w = XCreateWindow(d, r, 0, 0, 1, 1, 1,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWBackPixel | CWOverrideRedirect, &swa);
	printf("selection window: 0x%lx\n", w);
	XStoreName(d, w, daemon ? WMNAMEDAEMON : WMNAME);

	XSelectInput(d, w, ExposureMask | StructureNotifyMask | \
		KeyPressMask | ButtonReleaseMask | PropertyChangeMask);

				/* flash window */

	f = XCreateWindow(d, r, 0, 0, 50, 10, 1,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWBackPixel | CWOverrideRedirect, &swa);
	printf("flash window: 0x%lx\n", f);
	XSelectInput(d, f, ExposureMask | StructureNotifyMask);

				/* print strings and instructions */

	printf("selected strings:\n");
	for (a = 0; a < num; a++)
		printf("%4s: %s\n", keylabel(a + 1), buffers[a]);
	printf("\nmiddle-click and press %s-", keylabel(1));
	printf("%s to paste one of them, or 'q' to quit\n", keylabel(num));

				/* load font and colors */

	wp.fs = XLoadQueryFont(d, font);

	XAllocNamedColor(d, DefaultColormapOfScreen(s), "black", &sc, &sc);
	wp.black = sc.pixel;
	XAllocNamedColor(d, DefaultColormapOfScreen(s), "white", &sc, &sc);
	wp.white = sc.pixel;

	wp.g = XCreateGC(d, w, 0, NULL);
	XSetFont(d, wp.g, wp.fs->fid);

	fp = wp;
	fp.g = XCreateGC(d, f, 0, NULL);
	XSetFont(d, fp.g, fp.fs->fid);

				/* get or acquire the primary selection */

	if (((continuous && RequestPrimarySelection(d, w)) ||
	    AcquirePrimarySelection(d, r, w, &t)) &&
	    ! continuous) {
		XCloseDisplay(d);
		return EXIT_FAILURE;
	}

				/* show the flash window on startup */

	ResizeWindow(d, f, wp.fs, num);
	WindowAtPointer(d, f);
	hide = starthide;
	message = NULL;
	XMapRaised(d, f);

				/* main loop */

	pending = False;
	showing = False;
	chosen = False;
	firefox = False;
	prev = None;
	last.tv_sec = 0;
	last.tv_usec = 0;
	key = -1;
	selected = -1;

	for (stayinloop = True, exitnext = False; stayinloop;) {
		XNextEvent(d, &e);

		if (e.type == Expose && e.xexpose.window == f) {
			printf("expose on the flash window\n");
			draw(d, f, &fp, buffers, num, selected, message);
			XFlush(d);
			usleep(hide);
			printf("unmapping the flash window\n");
			XUnmapWindow(d, f);
			message = NULL;
			continue;
		}
		if (e.type == KeyPress && ! showing) {
			printf("keycode: %d\n", e.xkey.keycode);
			k = XLookupKeysym(&e.xkey, 0);
			printf("k: %c\n", (unsigned char) k);
			switch (k) {
			case XK_F1:
				if (showing) {
					XUnmapWindow(d, w);
					// -> UnmapNotify
					continue;
				}
				e.type = ShowWindow;
				// -> ShowWindow
				break;
			}
		}
		if (! ShortTime(&flashtime, hide, False)) {
			printf("short time expired, hiding flash window\n");
			XUnmapWindow(d, f);
		}

		switch (e.type) {
		case SelectionRequest:
			printf("selection request ");
			printf("from 0x%lX, ", e.xselectionrequest.requestor);
			PrintAtomName(d, e.xselectionrequest.target);
			printf("\n");

			re = &e.xselectionrequest;

					/* request from self */

			if (e.xselectionrequest.requestor == w) {
				printf("request from self, refusing\n");
				RefuseSelection(d, re);
				break;
			}

					/* request for TARGETS */

			if (re->target == XInternAtom(d, "TARGETS", True)) {
				SendSelection(d, t, re, NULL, 0, False);
				break;
			}

					/* request from firefox */

			if (! click && re->target == XInternAtom(d,
					"text/x-moz-text-internal", True)) {
				printf("\nWARNING: request from firefox\n");
				printf("\ttimeout expired: 1/2 second\n");
				printf("\tsee man page for details\n\n");
				firefox = True;
			}

					/* request for unsupported type */

			if (UnsupportedSelection(d, re->target, False)) {
				printf("unsupported selection type\n");
				RefuseSelection(d, re);
				break;
			}

					/* window is on screen */

			if (showing) {
				printf("window on screen, refusing request\n");
				RefuseSelection(d, re);
				break;
			}

					/* second request from firefox */

			if (firefox) {
				printf("firefox again, repeating answer\n");
				AnswerSelection(d, t, re,
					buffers, separator, key, False);
				firefox = False;
				ShortTime(&last, interval, True);
				break;
			}

					/* a string was chosen */

			if (click && chosen) {
				printf("request after choice, sending\n");
				chosen = False;
				AnswerSelection(d, t, re,
					buffers, separator, key, False);
				pending = False;
				ShortTime(&last, interval, True);
				break;
			}

					/* request in a short time */

			if (ShortTime(&last, interval, False)) {
				printf("short time, repeating answer\n");
				AnswerSelection(d, t, re,
					buffers, separator, key, False);
				ShortTime(&last, interval, True);
				break;
			}

					/* send middle-click, not selection */

			if (click)
				RefuseSelection(d, re);

					/* store request */

			request = *re;
			pending = True;

			/* fallthrough */

		case ShowWindow:

					/* position for later middle-click */

			if (click) {
				PointerPosition(d, r, &x, &y);
				printf("saved x=%d y=%d\n", x, y);
			}

					/* save focus window */

			XGetInputFocus(d, &pprev, &pret);
			if (prev == None && pprev != w) {
				prev = pprev;
				ret = pret;
				printf("previous focus: 0x%lX\n", prev);
			}

					/* map window */

			ResizeWindow(d, w, wp.fs, num);
			WindowAtPointer(d, w);
			XMapRaised(d, w);
			// -> MapNotify
			// -> Expose
			break;

		case Expose:
			printf("expose\n");
			draw(d, w, &wp, buffers, num, selected, NULL);
			XSetInputFocus(d, w, RevertToNone, CurrentTime);
			// grab pointer to disallow the other client from
			// making further requests
			XGrabPointer(d, w, True, 0,
				GrabModeAsync, GrabModeAsync,
				False, None, CurrentTime);
			break;

		case SelectionNotify:
			printf("selection notify\n");
			if (e.xselection.property == None)
				break;
			if (num >= MAXNUM)
				break;
			buffers[num] = GetSelection(d, w,
				e.xselection.selection, e.xselection.target);
			if (buffers[num] != NULL) {
				printf("selection added: %s\n", buffers[num]);
				num++;
			}
			if (num >= 2 || continuous)
				if (AcquirePrimarySelection(d, r, w, &t)) {
					XCloseDisplay(d);
					return EXIT_FAILURE;
				}

			ResizeWindow(d, f, wp.fs, num);
			if (showing) {
				XGetGeometry(d, w, &r, &xb, &yb,
					&dm, &dm, &dm, &dm);
				XMoveWindow(d, f, xb, yb);
				XUnmapWindow(d, w);
				// -> UnmapNotify
			}
			else
				WindowAtPointer(d, f);
			hide = changehide;
			XMapRaised(d, f);
			ShortTime(&flashtime, 0, True);
			// -> Expose on the flash window
			break;

		case KeyPress:
			printf("keycode: %d\n", e.xkey.keycode);
			k = XLookupKeysym(&e.xkey, 0);
			printf("k: %c\n", (unsigned char) k);
			printf("pending: %d\n", pending);
			key = keyindex(k);
			printf("key index: %d\n", key);
			keep = False;
			changed = False;
			if (key >= 0 && key < num && request.requestor != w)
				printf("pasting %s\n", buffers[key]);
			else if (k == XK_Up || k == XK_Down) {
				if (num == 0)
					break;
				selected = selected + (k == XK_Up ? -1 : +1);
				selected = (selected + num) % num;
				if (immediate)
					key = selected;
				else {
					XClearArea(d, w, 0, 0, 0, 0, True);
					break;
				}
			}
			else if (k == XK_Return || k == XK_KP_Enter) {
				if (num == 0 || selected == -1)
					break;
				key = selected;
			}
			else {
				key = -1;
				switch (k) {
				case 'z':
				case XK_F2:
					printf("add new selection %d\n", num);
					if (num >= MAXNUM)
						break;
					if (! RequestPrimarySelection(d, w)) {
						hide = messagehide;
						message = selectmessage;
						XMapRaised(d, f);
					}
					// -> SelectionNotify
					break;
				case XK_BackSpace:
				case XK_Delete:
					if (selected == -1) {
						printf("no string selected\n");
						break;
					}
					printf("delete %s\n", buffers[selected]);
					free(buffers[selected]);
					for (a = selected; a < num - 1; a++)
						buffers[a] = buffers[a + 1];
					num--;
					if (num > 0 || daemon)
						keep = True;
					else
						changed = True;
					break;
				case 's':
				case XK_F3:
					printf("delete last selection\n");
					if (num > 0) {
						num--;
						free(buffers[num]);
					}
					if (daemon)
						keep = True;
					else
						changed = True;
					break;
				case 'q':
				case XK_F5:
					if (showing)
						exitnext = True;
					else
						stayinloop = False;
					/* fallthrough */
				case 'd':
				case XK_F4:
					printf("delete all selections\n");
					for (a = 0; a < num; a++)
						free(buffers[a]);
					num = 0;
					changed = True;
					break;
				}
				if (selected >= num)
					selected = num - 1;
			}
			printf("index: %d\n", key);
			
			if (keep) {
				printf("keep window open\n");
				ResizeWindow(d, w, wp.fs, num);
				draw(d, w, &wp, buffers, num, selected, NULL);
				break;
			}

			XUnmapWindow(d, w);
			// -> UnmapNotify

			if (! changed || exitnext || ! stayinloop)
				break;

			printf("window changed, showing the flash window\n");
			XGetGeometry(d, w, &r, &xb, &yb, &dm, &dm, &dm, &dm);
			XMoveWindow(d, f, xb, yb);
			ResizeWindow(d, f, wp.fs, num);
			XMapRaised(d, f);
			hide = changehide;
			ShortTime(&flashtime, 0, True);
			// -> Expose on the flash window

			break;

		case ButtonRelease:
			printf("button release\n");
			xb = e.xbutton.x;
			yb = e.xbutton.y;
			printf("x=%d y=%d\n", xb, yb);
			il = wp.fs->ascent + wp.fs->descent;
			key = yb / il - 1;
			if (key == -1 && ! daemon) {
				XGetWindowAttributes(d,
					e.xbutton.window, &wa);
				if (xb >= wa.width - 6 - 2 * il &&
				    xb <= wa.width - il - 3 &&
				    ! pending) {
					printf("add new selection %d\n", num);
					if (num >= MAXNUM)
						break;
					RequestPrimarySelection(d, w);
					// -> SelectionNotify
				}
				if (xb >= wa.width - il)
					exitnext = True;
			}

			XUnmapWindow(d, e.xbutton.window);
			// -> UnmapNotify
			break;

		case KeyRelease:
			printf("keyrelease\n");
			break;

		case UnmapNotify:
			printf("unmap notify: ");
			if (e.xunmap.event == w)
				printf("multiselect window\n");
			else if (e.xunmap.event == f)
				printf("flash window\n");
			else
				printf("unknown window\n");
			if (prev == None)
				printf("no previous focus owner\n");
			else {
				XGetInputFocus(d, &pprev, &pret);
				printf("revert focus 0x%lX -> 0x%lX\n",
					pprev, prev);
				XSetInputFocus(d, prev, ret, CurrentTime);
				prev = None;
			}
			if (e.xunmap.window == w)
				showing = False;
			if (e.xunmap.window == f && (num == 0 && ! daemon)) {
				stayinloop = 0;
				break;
			}
			XUngrabPointer(d, CurrentTime);
			if (exitnext) {
				printf("exiting\n");
				stayinloop = False;
				break;
			}
			if (! pending && ! force)
				break;
			ShortTime(&last, interval, True);
			if (! click) {
				printf("sending selection\n");
				AnswerSelection(d, t, &request,
					buffers, separator, key, False);
				pending = False;
			}
			else if (key != -1) {
				printf("sending middle button click\n");
				chosen = True;

				printf("restore x=%d y=%d\n", x, y);
				XWarpPointer(d, None, r, 0, 0, 0, 0, x, y);
				XTestFakeButtonEvent(d, 2, True, CurrentTime);
				XTestFakeButtonEvent(d, 2, False, 100);
				pending = True;
			}
			break;

		case SelectionClear:
			printf("selection clear from ");
			printf("0x%lX\n", e.xselection.requestor);
			XUngrabPointer(d, CurrentTime);
			if (exitnext) {
				printf("exit next\n");
				break;
			}
			if (! daemon) {
				printf("no daemon mode, exit next\n");
				exitnext = 1;
			}
			if (! continuous)
				break;
			if (num >= MAXNUM)
				break;
			if (! RequestPrimarySelection(d, w)) {
				printf("requesting the primary selection\n");
				hide = messagehide;
				message = selectmessage;
				XMapRaised(d, f);
			}
			// -> SelectionNotify
			break;

		case PropertyNotify:
			printf("property notify ");
			printf("window 0x%lX ", e.xproperty.window);
			printf("state %d\n", e.xproperty.state);
			break;

		case MapNotify:
			printf("map notify: ");
			if (e.xmap.event == w)
				printf("multiselect window\n");
			else if (e.xmap.event == f)
				printf("flash window\n");
			else
				printf("unknown window\n");
			if (e.xmap.window == w)
				showing = True;
			break;

		case MapRequest:
			printf("map request\n");
			break;

		case ReparentNotify:
			printf("reparent notify\n");
			break;

		case ConfigureNotify:
			printf("configure notify\n");
			break;

		case ConfigureRequest:
			printf("configure request\n");
			break;

		default:
			printf("other event (%d)\n", e.type);
		}

		fflush(stdout);
	}

	// disown the selection so that the requestor does not ask it again
	// with a different conversion
	printf("diswon the selection\n");
	XSetSelectionOwner(d, XA_PRIMARY, None, CurrentTime);

	XDestroyWindow(d, w);
	XCloseDisplay(d);

	return EXIT_SUCCESS;
}

