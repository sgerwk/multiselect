/*
 * multiselect.c
 *
 * provide multiple selections in X11
 */

/*
 * todo:
 * - allow to choose the string to send by the mouse (right click to refuse);
 *   make a subwindow for every string that can be pasted, use events of type
 *   XCrossingEvent to detect enter/leave, for hightlighting and chosing
 * - allow for more than 9 strings, with keys a,b,c,...
 * - the current selections can only be shown and changed when another
 *   program tries to paste one; some programs can copy text but never paste
 *   it (e.g., pdf viewers) or paste it only in certain points (web browser);
 *   use another key combination to show and change the selections?
 * - multiselect only makes a single request for the selection when adding one;
 *   the result may be only an initial part of the selection if it is long
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
 * an alternative that is also consistent with this is to send a middle-click
 * to the requestor window after unmapping the window; this should causes
 * firefox to ask the selection again, which is served immediately; this
 * mechanism requires the current pointer coordinates to be saved and restored
 * before sending the middle click, and relies on it not moving too much
 * between the original click and the moment this program receives the
 * selection request
 *
 * an alternative solution is a preload library for increasing the timeout in a
 * select system call
 */

/*
 * the flash window
 *
 * when the user adds a selection by ctrl-shift-z, the selections are briefly
 * shown to confirm that the addition succeded
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

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

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

	if (store)
		*last = now;
	printf("shorttime: %s\n", ret ? "True" : "False");
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
	y = y + 10 + height + 2 * border < rheight ?
		y + 10 :
		y - 10 - (int) height;

	XMoveWindow(d, w, x, y);
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
 * acquire ownership of the primary selection
 */
Bool AcquirePrimarySelection(Display *d, Window root, Window w, Time *t) {
	Window o;

	XSetSelectionOwner(d, XA_PRIMARY, w, CurrentTime);
	o = XGetSelectionOwner(d, XA_PRIMARY);
	if (o != w) {
		printf("Cannot get selection ownership\n");
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

	printf("storing selection: %s\n", chars);
	if (re->target == XInternAtom(d, "TARGETS", True)) {
		targetlen = 0;
		targetlist[targetlen++] = XInternAtom(d, "STRING", True);
		if (! stringonly)
			targetlist[targetlen++] =
				XInternAtom(d, "UTF8_STRING", True);
		XChangeProperty(d, re->requestor, re->property, // re->target,
			XInternAtom(d, "ATOM", True), 32,
			PropModeReplace,
			(unsigned char *) &targetlist, targetlen);
	}
	else
		XChangeProperty(d, re->requestor, re->property, re->target, 8,
			PropModeReplace,
			(unsigned char *) chars, nchars);

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
	for (i = 0; i < nitems; i++)
		printf("%c", string[i]);
	printf("\n");

	r = strdup((char *) string);
	XFree(string);
	return r;
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
		char *buffers[], int n) {
	Window r;
	int x, y;
	unsigned int width, height, bw, depth, nwidth;
	int lpos, interline;
	int i;
	char num[10], help[] = "multiselect";

	XClearWindow(d, w);
	XSetBackground(d, wp->g, wp->white);
	XSetForeground(d, wp->g, wp->black);
	XGetGeometry(d, w, &r, &x, &y, &width, &height, &bw, &depth);

	interline = wp->fs->ascent + wp->fs->descent;
	lpos = wp->fs->ascent;

	for (i = -1; i < n; i++) {
		if (i == -1)
			XDrawString(d, w, wp->g, 0, lpos,
				help, MIN(sizeof(help) - 1, 100));
		else {
			sprintf(num, "%d ", i + 1);
			XDrawString(d, w, wp->g, 0, lpos, num, strlen(num));
			nwidth = XTextWidth(wp->fs, num, strlen(num));
			XDrawString(d, w, wp->g, nwidth, lpos,
				buffers[i], MIN(strlen(buffers[i]), 100));
		}
		XDrawLine(d, w, wp->g,
			0, lpos + wp->fs->descent,
			width, lpos + wp->fs->descent);
		lpos += interline;
	}
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
	int interval = 50000;
	Bool exitnext, stayinloop, pending, firefox;
	XEvent e;
	XSelectionRequestEvent *re, request;
	KeySym k;
	Window prev, pprev;
	int ret, pret;
	int key;

	int opt;
	int daemon = 0, daemonother;
	char **buffers, separator, *terminator;
	int a, num, size = 9;

				/* parse arguments */

	while (-1 != (opt = getopt(argc, argv, "dt:"))) {
		switch (opt) {
		case 'd':
			daemon = 1;
			break;
		case 't':
			separator = optarg[0];
			break;
		default:
			exit(EXIT_FAILURE);
		}
	}
	argc -= optind - 1;
	argv += optind - 1;
	if (daemon && argc - 1 != 0) {
		printf("daemon mode: ");
		printf("no string allowed from the command line\n");
		exit(EXIT_FAILURE);
	}
	else if (argc - 1 == 1 && ! strcmp(argv[1], "-")) {
		printf("reading selections from stdin\n");
		buffers = malloc(size * sizeof(char *));
		for (num = 0; num < 9; num++) {
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
		num = MIN(argc - 1, 9);
		buffers = malloc(size * sizeof(char *));
		for (a = 0; a < num; a++)
			buffers[a] = strdup(argv[a + 1]);
	}

				/* open display */

	d = XOpenDisplay(NULL);
	if (d == NULL) {
		printf("Cannot open display %s\n", XDisplayName(NULL));
		exit(EXIT_FAILURE);
	}
	s = DefaultScreenOfDisplay(d);
	r = DefaultRootWindow(d);

				/* run or not, daemon or not */

	daemonother = WindowNameExists(d, r, WMNAMEDAEMON);
	if (WindowNameExists(d, r, WMNAME) || (daemon && daemonother)) {
		printf("%s already running\n", WMNAME);
		XCloseDisplay(d);
		exit(EXIT_FAILURE);
	}
	if (daemon || ! daemonother)
		XGrabKey(d, XKeysymToKeycode(d, XK_z), ControlMask | ShiftMask,
			r, False, GrabModeAsync, GrabModeAsync);

				/* create the window, set font and input */

	swa.background_pixel = WhitePixelOfScreen(s);
	swa.override_redirect = True;
	w = XCreateWindow(d, r, 0, 0, 1, 1, 1,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWBackPixel | CWOverrideRedirect, &swa);
	printf("selection window: 0%lx\n", w);
	XStoreName(d, w, daemon ? WMNAMEDAEMON : WMNAME);

	XSelectInput(d, w, ExposureMask | StructureNotifyMask | \
		KeyPressMask | PropertyChangeMask);

				/* flash window */

	f = XCreateWindow(d, r, 0, 0, 50, 10, 1,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWBackPixel | CWOverrideRedirect, &swa);
	printf("flash window: 0%lx\n", f);
	XSelectInput(d, f, ExposureMask);

				/* print strings and instructions */

	printf("selected strings:\n");
	for (a = 0; a < num; a++)
		printf("%4d: %s\n", a + 1, buffers[a]);
	printf("\nmiddle-click and press %d-%d to paste one of them, ", 1, num);
	printf("or 'q' to quit\n");

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

				/* acquire primary selection */

	if (AcquirePrimarySelection(d, r, w, &t)) {
		XCloseDisplay(d);
		return EXIT_FAILURE;
	}

				/* main loop */

	pending = False;
	firefox = False;
	prev = None;
	last.tv_sec = 0;
	last.tv_usec = 0;
	key = 0;

	for (stayinloop = True, exitnext = False; stayinloop;) {
		XNextEvent(d, &e);

		if (e.type == Expose && e.xexpose.window == f) {
			printf("expose on the flash window\n");
			draw(d, f, &fp, buffers, num);
			XFlush(d);
			usleep(500000);
			XUnmapWindow(d, f);
			continue;
		}
		if (! ShortTime(&flashtime, 500000, False))
			XUnmapWindow(d, f);

		switch (e.type) {
		case SelectionRequest:
			printf("selection request, ");
			PrintAtomName(d, e.xselectionrequest.target);
			printf("\n");

			re = &e.xselectionrequest;

					/* request for TARGETS */

			if (re->target == XInternAtom(d, "TARGETS", True)) {
				SendSelection(d, t, re, NULL, 0, False);
				break;
			}

					/* request from firefox */

			if (re->target == XInternAtom(d,
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

					/* pending request */

			if (pending) {
				printf("pending request, refusing this\n");
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

					/* request in a short time */

			if (ShortTime(&last, interval, True)) {
				printf("short time, repeating answer\n");
				AnswerSelection(d, t, re,
					buffers, separator, key, False);
				break;
			}

					/* save focus window */

			XGetInputFocus(d, &pprev, &pret);
			if (prev == None && pprev != w) {
				prev = pprev;
				ret = pret;
				printf("previous focus: 0x%lX\n", prev);
			}

					/* store request and map window */

			request = *re;
			pending = True;
			ResizeWindow(d, w, wp.fs, num);
			WindowAtPointer(d, w);
			XMapRaised(d, w);
			// -> Expose
			break;

		case Expose:
			printf("expose\n");
			draw(d, w, &wp, buffers, num);
			XSetInputFocus(d, w, RevertToNone, CurrentTime);
			// grab pointer to disallow the other client from
			// making further requests
			XGrabPointer(d, w, True, 0,
				GrabModeAsync, GrabModeAsync,
				False, None, CurrentTime);
			break;

		case SelectionNotify:
			if (e.xselection.property == None)
				break;
			if (num >= size)
				break;
			buffers[num] = GetSelection(d, w,
				e.xselection.selection, e.xselection.target);
			if (buffers[num] != NULL)
				num++;
			if (num >=2 && AcquirePrimarySelection(d, r, w, &t)) {
				XCloseDisplay(d);
				return EXIT_FAILURE;
			}

			WindowAtPointer(d, f);
			ResizeWindow(d, f, wp.fs, num);
			XMapRaised(d, f);
			ShortTime(&flashtime, 0, True);
			break;

		case KeyPress:
			printf("key: %d\n", e.xkey.keycode);
			k = XLookupKeysym(&e.xkey, 0);
			if (e.xkey.window == r && k == 'z') {
				printf("add new selection\n");
				if (num < size)
					XConvertSelection(d, XA_PRIMARY,
						XA_STRING, XA_PRIMARY,
						w, CurrentTime);
				break;
			}
			if (! pending) {
				printf("no pending request\n");
				break;
			}
			if ((int) k - '1' >= 0 && (int) k - '1' < num &&
			    request.requestor != w) {
				key = k - '1';
				printf("pasting %s\n", buffers[key]);
			}
			else {
				key = -1;
				switch (k) {
				case 's':
					printf("delete last selection\n");
					if (num > 0) {
						num--;
						free(buffers[num]);
					}
					if (num > 0)
						break;
					// retain the selection if num==1 since
					// the previous owner has already lost
					// it at this point
					XSetSelectionOwner(d, XA_PRIMARY,
						None, CurrentTime);
					break;
				case 'd':
				case 'q':
					printf("delete all selections\n");
					for (a = 0; a < num; a++)
						free(buffers[a]);
					num = 0;
					// disown selection even when exiting
					// to avoid the requestor to ask it
					// again with a different conversion
					XSetSelectionOwner(d, XA_PRIMARY,
						None, CurrentTime);
					if (k == 'q' && ! daemon)
						exitnext = True;
					break;
				}
			}

			ShortTime(&last, interval, True);
			AnswerSelection(d, t, &request,
				buffers, separator, key, False);
			pending = False;
			XUnmapWindow(d, w);
			// -> UnmapNotify
			break;

		case KeyRelease:
			printf("keyrelease\n");
			break;

		case UnmapNotify:
			printf("unmap\n");
			if (prev == None && ! exitnext) {
				XUngrabPointer(d, CurrentTime);
				break;
			}
			XGetInputFocus(d, &pprev, &pret);
			printf("revert focus 0x%lX -> 0x%lX\n", pprev, prev);
			XSetInputFocus(d, prev, ret, CurrentTime);
			XUngrabPointer(d, CurrentTime);
			prev = None;
			if (exitnext) {
				printf("exiting\n");
				stayinloop =  False;
			}
			break;

		case SelectionClear:
			printf("selection clear\n");
			if (exitnext)
				break;
			XUngrabPointer(d, CurrentTime);
			if (! daemon && daemonother)
				stayinloop = 0;
			break;

		case PropertyNotify:
			printf("property notify ");
			printf("window 0x%lX ", e.xproperty.window);
			printf("state %d\n", e.xproperty.state);
			break;

		case MapNotify:
			printf("map notify\n");
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

	XDestroyWindow(d, w);
	XCloseDisplay(d);

	return EXIT_SUCCESS;
}

