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
 * - allow more than 9 strings, with keys a,b,c,...
 * - data from selection: when another program selects something, get the
 *   selection, split it by lines and acquire the selection back
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
 * well as any following one arrived in a short time; when this happens, the
 * other client may decide the use the cut buffer
 *
 * this is wrong because the user has decided not to paste anything, and
 * certainly is not expecting a string not in the list to be pasted; this is
 * why multiselect deletes the cut buffer at startup
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#define MIN(a,b) (((a) < (b)) ? (a) : (b))

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
Bool ShortTime(struct timeval *last) {
	struct timeval now;
	int interval = 50000;
	Bool ret = True;

	gettimeofday(&now, NULL);

	if (now.tv_sec >= last->tv_sec + 2)
		last->tv_sec = now.tv_sec - 2;
	ret = now.tv_usec + 1000000 * (now.tv_sec - last->tv_sec)
	      <= last->tv_usec + interval;

	*last = now;
	printf("shorttime: %s\n", ret ? "True" : "False");
	return ret;
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
Bool AcquirePrimarySelection(Display *d, Window w, Time *t) {
	Window o;

	XSetSelectionOwner(d, XA_PRIMARY, w, CurrentTime);
	o = XGetSelectionOwner(d, XA_PRIMARY);
	if (o != w) {
		printf("Cannot get selection ownership\n");
		return True;
	}
	if (t != NULL)
		*t = GetTimestampForNow(d, w);
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
 * answer a request for selection
 */
Bool AnswerSelection(Display *d, Time t, XSelectionRequestEvent *request,
		char **buffers, int key, int stringonly) {
	if (key == -1) {
		RefuseSelection(d, request);
		return False;
	}
	else
		return SendSelection(d, t, request,
			buffers[key], strlen(buffers[key]), stringonly);
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
	char num[10];

	XClearWindow(d, w);
	XSetBackground(d, wp->g, wp->white);
	XSetForeground(d, wp->g, wp->black);
	XGetGeometry(d, w, &r, &x, &y, &width, &height, &bw, &depth);

	interline = wp->fs->ascent + wp->fs->descent;
	lpos = wp->fs->ascent;

	for (i = 0; i < n; i++) {
		sprintf(num, "%d ", i + 1);
		XDrawString(d, w, wp->g, 0, lpos, num, strlen(num));
		nwidth = XTextWidth(wp->fs, num, strlen(num));
		XDrawString(d, w, wp->g, nwidth, lpos,
			buffers[i], MIN(strlen(buffers[i]), 100));
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
	Window r, w;
	XColor sc;
	char *font = "-misc-*-medium-*-*-*-18-*-*-*-*-*-iso10646-1";
	struct WindowParameters wp;
	char *wmname = "multiselect";
	XSetWindowAttributes swa;
	unsigned int width, height;

	Time t;
	struct timeval last;
	Bool exitnext, stayinloop, pending, firefox;
	XEvent e;
	XSelectionRequestEvent *re, request;
	KeySym k;
	Window prev, pprev;
	int ret, pret;
	int key;

	char **buffers, *terminator;
	int a, num;

				/* parse arguments */

	if (argc - 1 < 1) {
		printf("no argument passed, reading strings from stdin\n");
		buffers = malloc(10 * sizeof(char *));
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
		buffers = malloc(num * sizeof(char *));
		for (a = 0; a < num; a++)
			buffers[a] = argv[a + 1];
	}

				/* open display */

	d = XOpenDisplay(NULL);
	if (d == NULL) {
		printf("Cannot open display %s\n", XDisplayName(NULL));
		exit(EXIT_FAILURE);
	}
	s = DefaultScreenOfDisplay(d);
	r = DefaultRootWindow(d);
	if (WindowNameExists(d, r, wmname)) {
		printf("%s already running\n", wmname);
		XCloseDisplay(d);
		exit(EXIT_FAILURE);
	}

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

				/* create the window, set font and input */

	width = 400;
	height = (wp.fs->ascent + wp.fs->descent) * num;
	w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, width, height,
		1, BlackPixelOfScreen(s), WhitePixelOfScreen(s));
	XStoreName(d, w, wmname);

	swa.override_redirect = True;
	XChangeWindowAttributes(d, w, CWOverrideRedirect, &swa);

	wp.g = XCreateGC(d, w, 0, NULL);
	XSetFont(d, wp.g, wp.fs->fid);

	XSelectInput(d, w, ExposureMask | StructureNotifyMask | \
		KeyPressMask | PropertyChangeMask);

				/* acquire primary selection */

	if (AcquirePrimarySelection(d, w, &t)) {
		XCloseDisplay(d);
		return EXIT_FAILURE;
	}

				/* remove the first cut buffer */

	XDeleteProperty(d, r, XInternAtom(d, "CUT_BUFFER0", True));

				/* main loop */

	pending = False;
	firefox = False;
	prev = None;
	last.tv_sec = 0;
	last.tv_usec = 0;
	key = 0;

	for (stayinloop = True, exitnext = False; stayinloop;) {
		XNextEvent(d, &e);

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
				AnswerSelection(d, t, re, buffers, key, False);
				firefox = False;
				ShortTime(&last);
				break;
			}

					/* request in a short time */

			if (ShortTime(&last)) {
				printf("short time, repeating answer\n");
				AnswerSelection(d, t, re, buffers, key, False);
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

		case KeyPress:
			printf("key: %d\n", e.xkey.keycode);
			if (! pending) {
				printf("no pending request\n");
				break;
			}
			k = XLookupKeysym(&e.xkey, 0);
			if ((int) k - '1' >= 0 && (int) k - '1' <= num) {
				key = k - '1';
				printf("pasting %s\n", buffers[key]);
			}
			else {
				key = -1;
				if (k == 'q') {
					exitnext = True;
					// disown selection so that the
					// requestor will not ask for it again
					// with a different conversion
					XSetSelectionOwner(d, XA_PRIMARY,
						None, CurrentTime);
				}
			}

			ShortTime(&last);
			AnswerSelection(d, t, &request, buffers, key, False);
			pending = False;
			XUnmapWindow(d, w);
			// -> UnmapNotify
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
			stayinloop = False;
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

