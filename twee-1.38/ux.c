
/* ux is an xlib only text editor.
 compile with:
 gcc -g -O2 -fomit-frame-pointer -pipe -Wall -o ux ux.c -L/usr/X11R6/lib -lX11
 sstrip ux
 Public Domain, (C) 2005 Terry Loveall, 
 THIS PROGRAM COMES WITH ABSOLUTELY NO WARRANTY OR BINARIES. COMPILE AND USE 
 AT YOUR OWN RISK.

 usage: ux <filename> # <filename> NOT optional

*/

#define VERSION "1.38"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#define XK_MISCELLANY
#include <X11/keysymdef.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>

#define BUF 1024*1024
#define MODE 0666
#define TABSZ 4
#define TABM TABSZ-1
#define MAXCOLS 80
#define MAXLINES 24

// #define FONTNAME "fixed"
#define FONTNAME "7x14"
#define KEYBUFLEN 20

char helpt[] =
"	ux version "VERSION"

left				<--			top of file			^Home
right				-->			bottom of file		^End
up					/\\			del char			Del
down				\\/			del prev char		Backspace
word left			^<--		del rest of line	^Y
word right			^-->		undo				^U
line begining		Home		write file			F2
line end			End			find				^F
pgdown				PageDown	quit				^Q
pgup				PageUp		help				F1
cut					Shift-Del	copy				^Ins
paste				Shift-Ins	jmp to line			^J

				Push any key to continue";

int line_count = 1;
int done;
int row, col;

int screen_width=MAXCOLS;	// default starting screen width
int screen_height=MAXLINES;	// default starting screen height

char ln[MAXCOLS];	// goto line string, used by gotoln()
char ss[MAXCOLS];	// search string, used by look()
char ubuf[BUF];		// undo buffer
char buf[BUF];		// the edit buffer
char *ebuf = buf + BUF;	// end of edit buffer
char *etxt = buf;		// end of edit text
char *curp = buf;		// current position in edit buffer
char *page, *epage;		// start and end of displayed edit text
char *filename;			// file being edited
int ch;				// most recent command/edit character

// undo record struct
typedef struct {
	char ch;
	int pos;
} U_REC;
U_REC* undop = (U_REC*)&ubuf;

typedef struct {
        int X;
        int Y;
} COORD;
COORD xy;

// func protos
void bell();
void set_title(char *str);
void highvideo();
void lowvideo();
void clrscr();
int paste_primary(int win, int property, int Delete);
int request_selection(int time);
char *mrealloc(char *s, int len);
void set_selection();
void send_selection(XSelectionRequestEvent * rq);
int getxtru(char* pos);
void iright();
void dleft();
void moveto();
void do_select(int delf);
void do_paste();
int getch();
void draw_cursor();
void put1(int c);
void gotoxy(int x, int y);
void clrtoeol();
void emitch(int c);
void addstr(char *p);
void mark_off();
void block_mark();
void oblk();
char *prevline(char *p);
char *nextline(char *p);
char *adjust(char *p, int column);
int scmp(char* t, char* s, int c);
void cmove(char *src, char *dest, int cnt);
// edit commands
void left();
void down();
void up();
void right();
void wleft();
void pgdown();
void pgup();
void wright();
void lnbegin();
void lnend();
void top();
void bottom();
void delete_one();
void delete();
void bksp();
void delrol();
void undo();
void writef();
void look();
void gotoln();
void quit();
void help();
void nop();
// high level interface
void handle_uekey(int ch);
void init();
void redraw();
int main(int argc, char **argv);

// command key array, '&0X1F' or '0x7fxx' means control key
int key[26] = {
0xff51,0xff54,0xff52,0x7f54,0x7f52,0xff53,
0x7f51,0xff56,0xff55,0x7f56,0x7f55,0x7f53,
0xff50,0xff57,0x7f50,0x7f57,
0xffff,0xff08,'Y'&0X1F,'U'&0X1F,
0xffbf,'F'&0X1F,'Q'&0X1F,'J'&0X1F,
0xffbe,'\0'
};

// command key function array, one to one correspondence to key array, above
void (*func[])() = {
	left, down, up, down, up, right, 
	wleft, pgdown, pgup, pgdown, pgup, wright,
	lnbegin, lnend, top, bottom, 
	delete, bksp, delrol, undo,
	writef, look, quit, gotoln, 
	help, nop
};

// X interface

int winwidth;	// window width in pixels
int winheight;	// window height in pixels

char *cur_pos;

int xtru = 0;			 		// file position
int y, x;

int buttonpressed = 0;

char *bstart, *bend;	// marked block start, end and char pointer
char *mk, *last_mk;				// mark

Display *dpy;
GC gc;
GC rectgc;
Window win;
XFontStruct *font;
int fwidth, fheight;    // font character width, height
int screen;
unsigned long white;
unsigned long black;

int curpos = 0;

XEvent event;
XKeyEvent *keve;
Time eve_time;
char *selection_text;	// selected text for X clipboard
int selection_length;

// support routines

void highvideo()
{
	XSetForeground(dpy, gc, black);
	XSetBackground(dpy, gc, white);
}

void lowvideo()
{
	XSetForeground(dpy, gc, white);
	XSetBackground(dpy, gc, black);
}

void clrscr() { XClearWindow(dpy,win); }

void bell()
{
	XBell(dpy,100);
}

void set_title(char *str)
{
	char b[256];

	sprintf(b, "%s :ux", str);
	XStoreName(dpy, win, b);
}

int paste_primary(int win, int property, int Delete)
{
	Atom actual_type;
	int actual_format, i;
	long nitem, bytes_after, nread;
	unsigned char *data;

	if(property == None)	// don't paste nothing
	return(0);

	nread = 0;
	// X-selection paste loop
	do {
		// get remaining selection max 1024 chars
		if(XGetWindowProperty
			(dpy, win, property, nread / 4, 1024, Delete,
			 AnyPropertyType, &actual_type, &actual_format, &nitem,
			 &bytes_after, (unsigned char **) &data)
			!= Success)
			return(0);
		// paste last batch one char at a time
		for(i = 0; i < nitem; handle_uekey((char)data[i++]));
		nread += nitem;
		XFree(data);
	} while (bytes_after > 0);
	redraw();
	return(nread);
}

int request_selection(int time)
{
	Window w;
	Atom property;

	if((w = XGetSelectionOwner(dpy, XA_PRIMARY)) == None) {
		int tmp = paste_primary(DefaultRootWindow(dpy), XA_CUT_BUFFER0, False);
		return(tmp);
	}
	property = XInternAtom(dpy, "VT_SELECTION", False);
	XConvertSelection(dpy, XA_PRIMARY, XA_STRING, property, win, time);
	return(0);
}

char *mrealloc(char *s, int len)
{
	char *ttt;
	if(!s) ttt = (char *) malloc(len);
	else ttt = (char *) realloc(s, len);
	return ttt;
}

void set_selection()
{
	int i;

	if(!mk || mk == cur_pos) return;

	if(cur_pos < mk)
		{ bstart = cur_pos; bend = mk; }
	else
		{ bstart = mk; bend = cur_pos; }

	selection_length = bend - bstart;
	if((selection_text = 
		(char *) mrealloc(selection_text, selection_length)) == NULL)
	{
		printf("realloc.\n");
se_exit:
		bell();
		return;
	}
	for (i = 0; i < selection_length; i++) {
		selection_text[i] = bstart[i] == 0 ? '\n' : bstart[i];
	}
	XSetSelectionOwner(dpy, XA_PRIMARY, win, (Time) eve_time);
	if(XGetSelectionOwner(dpy, XA_PRIMARY) != win) {
		printf("Cant select.\n");
		goto se_exit;
	}
	XChangeProperty(dpy, DefaultRootWindow(dpy), XA_CUT_BUFFER0,
		XA_STRING, 8, PropModeReplace, selection_text,
		selection_length);
}

void send_selection(XSelectionRequestEvent * rq)
{
	XSelectionEvent notify;

	notify.type = SelectionNotify;
	notify.display = rq->display;
	notify.requestor = rq->requestor;
	notify.selection = rq->selection;
	notify.target = rq->target;
	notify.time = rq->time;
	XChangeProperty(dpy, rq->requestor, rq->property, XA_STRING, 8,
		PropModeReplace, selection_text, selection_length);
	notify.property = rq->property;
	XSendEvent(dpy, rq->requestor, False, 0, (XEvent *) & notify);
}

int getxtru(char* pos)
{
	int i = 1;
	char* tmpp;
	if((pos > buf) || (pos < etxt)) {
		tmpp = prevline(pos);
		while((tmpp != pos) && (pos > buf) && (pos < etxt))
		{
			if(*tmpp++ == '\t') i = i + ((TABSZ) - ((i-1)%TABSZ));
			else i++;
		}
	}
	return(i);
}

void iright()
{
	right();
	col++;
	xtru = getxtru(curp);
}

void dleft()
{
	left();
	col--;
	xtru = getxtru(curp);
}

void moveto()
{
	int old, newx, newy;

	newx = ((event.xbutton.x < 0 ? 0 : event.xbutton.x)/fwidth) + 1;
	newy = ((event.xbutton.y-3)/fheight) - 1;
	newy = (newy >= line_count) ? line_count : newy;

	if((row+1 == newy && col == newx) ) return;

	while((row < newy) && (curp < etxt)) { down(); row++; }
	while(row > newy) { up(); row--; }

	// detect impossible to achieve x pos within tabs
	xtru = getxtru(curp);
	old = xtru - newx;
	while((curp < etxt) &&
			(old > 0 ? xtru > newx : (xtru < newx && *curp != '\n')))
	{
		xtru < newx ? iright() : dleft();
	}
}

void do_select(int delf)
{
	if(mk && mk != cur_pos) {
		set_selection();
		if(delf) delete();
	}
	mark_off();
	redraw();
}

void do_paste()
{
	if(mk) delete();
	request_selection(event.xbutton.time);
}

void draw_cursor()
{
	// draw the cursor
	XDrawLine(dpy, win, gc, 
				2 + (xy.X * fwidth), 
				(fheight * ((xy.Y)+1)) + 2,
				// horizontal cursor
//				2 + (xy.X * fwidth) + fwidth,
//				(fheight * ((xy.Y)+1)) + 2
				// vertical cursor
				2 + (xy.X * fwidth),
				(fheight * xy.Y) + 2
				);
}

void addstr(char *p)
{
	while(*p) put1(*p++);
}

void mark_off()
{
	mk = last_mk = NULL;
}

void block_mark()
{
	if( mk == NULL ) {
		last_mk = mk = curp;
	}
	else mark_off();
}

void oblk()
{
	// if shift not pressed turn off marked block
	if(buttonpressed) return;
	if(!(event.xkey.state & ShiftMask) && mk) mark_off();
}

void gotoxy(int x, int y){
	xy.Y=y;xy.X=x;
}

int getch()
{
#define DBLCLICK 500
	int i;
	// main event interpreter, dispatch function calls in response to events
	for(;;)
	{
		static Time button_release_time;
		int y0 = 0, y1 = 0, yf, yt;

		XNextEvent(dpy,&event);

		switch(event.type)	{
		case Expose:
			while(XCheckTypedEvent(dpy,Expose,&event));
			redraw();
			break;
		case MotionNotify:
/*			if(buttonpressed==-1) {
//				scroll_text(event.xbutton.y);
				break;
			}
*/
			if(buttonpressed) {
				i = 128;
				while(!XCheckTypedWindowEvent(dpy, win, MotionNotify, &event) && i--) ;
				if(!mk) block_mark();
				moveto();
				if(event.xbutton.y < fheight+2) {
					cur_pos = curp;
					down();
				} else if(event.xbutton.y >= winheight-6-font->descent) {
					cur_pos = curp;
					up();
				} else {
					yf = y0;
					if(y<yf) yf = y;
					if(y1<yf) yf = y1;
					yt = y0;
					if(y>yt) yt = y;
					if(y1>yt) yt = y1;
					cur_pos = curp;
					y1 = y;
				}
				redraw();
			}
			break;
		case ButtonPress:
/*
	        if(event.xbutton.x>=winwidth-10) {
				buttonpressed = -1;
//				scroll_text(event.xbutton.y);
				break;
			}
*/
			buttonpressed = event.xbutton.button;
			if(event.xbutton.time - eve_time < DBLCLICK) break;
			eve_time = event.xbutton.time;
			if(event.xbutton.button == Button1) mark_off(); // unmark
			if(!mk){
				moveto();
				y0 = y;
				cur_pos = curp;
				block_mark();	// start new mark
			}
			redraw();
			break;
		case ButtonRelease:
/*			if(buttonpressed == -1) {
				buttonpressed = 0;
				break;
			}
*/
			buttonpressed = 0;
			switch (event.xbutton.button) {
				case Button1:
					if(event.xbutton.time - button_release_time < DBLCLICK) {
						mark_off();
					}
					else
					{
						button_release_time = event.xbutton.time;
						if(mk == cur_pos) mark_off();
					}
					goto setcursor;
				case Button3:
					if((event.xbutton.time - eve_time < DBLCLICK) &&
						(event.xbutton.time - eve_time)) {
						do_paste();
						goto nosetcursor;
					}
					else
					{
						button_release_time = event.xbutton.time;
						set_selection();
						mark_off(); 	// unmark
					}
					goto setcursor;
				case Button2:
					if((event.xbutton.time - eve_time < DBLCLICK) &&
						(event.xbutton.time - eve_time)) {
						do_paste();
					}
					goto setcursor;
			}
//			break;
setcursor:
			moveto();
nosetcursor:
			cur_pos = curp;
			redraw();
			break;
		case KeyPress:{
				int count;
				char astr[10];
				KeySym skey;

				i = 128;
				while(!XCheckTypedWindowEvent(dpy, win, KeyPress, &event) && i--) ;

				eve_time = keve->time;
				astr[0] = 0;
				count = XLookupString(keve, astr, sizeof (astr), &skey, NULL);
				astr[count] = 0;

				// convert X-ascii and key state to control chars
				if((skey & 0xfff0) == 0xffe0) continue;	// ignore Ctrl/Sh/Alt

				// do paste on Shift-Ins
				if((skey == 0xff63) && (keve->state & ShiftMask)) do_paste();

				// do_select w/o delete on Ctrl-Ins
				else if((skey == 0xff63) && (keve->state & ControlMask))
						{ do_select(0);}

				// do_select w/delete on Shift-Del
				else if((skey == 0xffff) && (keve->state & ShiftMask))
						{ do_select(1);}

				// process normal ASCII
				else
				{
					if(keve->state & ControlMask) {
						if(skey > 0xff00) skey &= 0x7fff;
						else if((skey <= 0x7f) && (skey >= ' ')) skey &= 0x1f;
						if(skey == '\b') skey = 0xff08;
					}
//printf("skey=<%x\n",skey);
					return(skey);
				}
			}
			break;
		case SelectionClear:
			break;
		case SelectionRequest:
			send_selection((XSelectionRequestEvent *) & event);
			break;
		case SelectionNotify:
			paste_primary(event.xselection.requestor, 
							event.xselection.property, True);
			break;
		case ConfigureNotify:
			i = 128;
			while(!XCheckTypedWindowEvent(dpy, win, ConfigureNotify, &event) && i--) ;
			winwidth = event.xconfigure.width;
			screen_width = (winwidth - 2)/fwidth;
			winheight = event.xconfigure.height;
			screen_height = (winheight - 3)/fheight;
			break;
		case DestroyNotify:
			XCloseDisplay(dpy);
            done = 1;
			exit(1);
		default:
		}
		break;
	}
	return(-1);
}

void put1(int c)
{
	XDrawImageString(dpy, win, gc, 
		(xy.X * fwidth) + 2, ((xy.Y)+1) * fheight, (char *)&c, 1);
	xy.X++;
}

void emitch(int c)
{
	if(c == '\t') do { put1(' '); } while(xy.X & (TABM));
	else put1(c != '\n' ? c : ' ');
	if(c == '\n') { xy.X=0; xy.Y++; };
}

void clrtoeol()
{
	int ox = xy.X;
	while(xy.X < screen_width) put1(' ');
	xy.X = ox;
}

char *prevline(char *p)
{
	while (buf < --p && *p != '\n');
	return (buf <= p ? ++p : buf);
}

char *nextline(char *p)
{
	while (p < etxt && *p++ != '\n');
	return (p);
}

char *adjust(char *p, int column)
{
	int i = 0;
	while (p < etxt && *p != '\n' && i < column) {
		i += *p++ == '\t' ? TABSZ-(i & (TABM)) : 1;
	}
	return (p);
}

int scmp(char* t, char* s, int c)
{
	int i;
	for( i=0; i < c; i++) if(s[i]-t[i]) return (s[i]-t[i]);
	return (0);
}

void cmove(char *src, char *dest, int cnt)
{
	if(src > dest) while(cnt--) *dest++ = *src++;
	if(src < dest){
		src += cnt;
		dest += cnt;
		while(cnt--) *--dest = *--src;
	}
	etxt += dest-src;
}

void left()
{
	if(buf < curp) --curp;
} 

void down()
{
	curp = adjust(nextline(curp), col);
}

void up()
{
	curp = adjust(prevline(prevline(curp)-1), col);
}

void right()
{
	if(curp < etxt) ++curp;
}

void wleft()
{
	while (isspace(*(curp-1)) && buf < curp) --curp;
	while (!isspace(*(curp-1)) && buf < curp) --curp;
}

void pgdown()
{
	page = curp = prevline(epage-1);
	while (0 < row--) down();
	epage = etxt;
}

void pgup()
{
	int i = screen_height-1;
	while (0 < --i) { page = prevline(page-1); up(); }
}

void wright()
{
	while (!isspace(*curp) && curp < etxt) ++curp;
	while (isspace(*curp) && curp < etxt) ++curp;
}

void lnbegin()
{
	curp = prevline(curp);
}

void lnend()
{
	curp = nextline(curp); left();
}

void top()
{
	curp = buf; xtru = x = 1;
}

void bottom()
{
	epage = curp = etxt;
}

void delete_one()
{
	if(curp < etxt){
		if(*curp == '\n') line_count--;
		if((char*)&undop[1] < ubuf+BUF){
			undop->ch = *curp;
			undop->pos = -(int)curp;	// negative means delete
			undop++;
		}
		cmove(curp+1, curp, etxt-curp);
	}
}

void delete()
{
	int i;
	char *s;
	if(mk) {
		if(bstart > bend) {s = bstart; bstart = bend; bend = s;}
		curp = bstart;
		i = bend-bstart;
		while(i--) delete_one();
		mark_off();
	} else delete_one();
}

void bksp()
{
	if(buf < curp) { left(); delete(); }
}

void delrol()
{
	if(*curp != '\n') do { delete_one(); } while(curp < etxt && *curp != '\n');
	else delete_one();
}

void undo()
{
	if((void*)undop > (void*)ubuf){
		undop--;
		curp = (char*)undop->pos;
		if(undop->pos < 0){	// negative means was delete
			(int)curp = -(int)curp;		// so insert
			cmove(curp, curp+1, etxt-curp);
			*curp = undop->ch;
			if(*curp == '\n') line_count++;
		}
		else{	// was insert so delete
			if(*curp == '\n') line_count--;
			cmove(curp+1, curp, etxt-curp);
		}
	}
}

void writef()
{
	int i;
	write(i = creat(filename, MODE), buf, (int)(etxt-buf));
	close(i);
	undop = (U_REC*)&ubuf;
}

int prompt(char *prompt, char *s, int ch)
{
	char c;
	int i = strlen(s);
	do {
		gotoxy(17,0);
		clrtoeol();
		addstr((void*)prompt);
		addstr((void*)s);
		gotoxy(40+i,0);
		while ((c = getch()) == -1);
		if(c == '\b') {
			if(i) s[--i] = 0;
			continue;
		}
		else {
			if(i == screen_width) continue;
			s[i++] = c;
		}
	} while(c != 0x1b && c != '\n' && c != '\r' && c != ch );
	s[--i] = 0;
	return(c == 0x1b ? 0 : i);
}

void look()
{
	int i = prompt(" Find: ", ss, 0x06);
	if(i){
		do { right(); } while(curp < etxt && scmp(curp,ss,i));
		if(curp < etxt) {
			bstart = mk = curp+(strlen(ss));
			bend = curp;
		} else bell();
	}
}

void goln(int i)
{
	top();
	while(--i > 0) down();
}

void gotoln()
{
	if(prompt(" goto line: ", ln, 0x0a)) goln(atoi(ln));
}

void quit()
{
	int c;
	if((void*)undop > (void*)ubuf) {
		gotoxy(3,0);
		addstr(" File changed. Save? y/n/Esc ");
		if((c = getch()) == 0xff1b) return;
		if(c == 'y') writef();
	}
	done = 1;
}

void help()
{
	char *s = helpt;
	// draw the black on black erase rectangle
	XFillRectangle(dpy, win, rectgc, 0, 0, winwidth, winheight);

	gotoxy(0,1);
	while(*s) emitch(*s++);
	getch();
}

void nop()
{
}

// high level interface

void handle_uekey(int ch)
{
	if(etxt < ebuf) {
		if(((ch < ' ') || (ch & 0x80)) &&
			((ch != '\t') && (ch != '\n') && (ch != '\r'))) return;
		if(mk) delete();
		cmove(curp, curp+1, etxt-curp);
		*curp = (ch & 0xff) == '\r' ? '\n' : ch;
		if(*curp++ == '\n') {
			line_count++;
			if(xy.Y == screen_height-1) epage = curp;
		}
		if((char*)&undop[1] < ubuf+BUF){
//			undop->ch = curp[-1];
			undop->pos = (int)curp-1;	// positive means insert
			undop++;
		}
	}
}

void init()
{
	XGCValues values;
	int valuemask = 0;
	char *display_name = NULL;
	int display_width, display_height;
	Cursor mouse_cursor;
	XSizeHints *win_size_hints;
	XSetWindowAttributes attrib;

	dpy = XOpenDisplay(display_name);

	if(dpy == NULL) exit(2); // Cant open DISPLAY

	screen = DefaultScreen(dpy);
	display_width = DisplayWidth(dpy, screen);
	display_height = DisplayHeight(dpy, screen);

	black = BlackPixel(dpy, screen);
	white = WhitePixel(dpy, screen);

	mouse_cursor = XCreateFontCursor(dpy, XC_left_ptr);

	// set font
	font = XLoadQueryFont(dpy, FONTNAME);
	if(!font) exit(3);	// Cant load font

	fwidth = XTextWidth(font,"8",1);
	winwidth = (fwidth * screen_width) + 2;
	fheight = font->ascent + font->descent;
	winheight = (fheight * screen_height) + 3;

	attrib.override_redirect = True;
	attrib.background_pixel = white;
	attrib.border_pixel = black;
	attrib.cursor = mouse_cursor;
	attrib.event_mask = KeyPressMask |
						StructureNotifyMask |
						EnterWindowMask |
						LeaveWindowMask |
						PointerMotionMask |
						ButtonPressMask |
						ButtonReleaseMask |
						ExposureMask   |
						Button1MotionMask |
						Button2MotionMask |
						Button3MotionMask |
						VisibilityChangeMask;
	attrib.override_redirect = True;
	win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
						20, 20, winwidth, winheight,
						1,
						WhitePixel(dpy,screen),BlackPixel(dpy,screen));

	// set up window hints
	win_size_hints = XAllocSizeHints();

	if(!win_size_hints) exit(4);	// Cant alloc hints

	win_size_hints->flags = PSize;
	win_size_hints->width = winwidth;

	win_size_hints->height = winheight;
	XSetWMNormalHints(dpy, win, win_size_hints);
	XFree(win_size_hints);
	XMapWindow(dpy, win);

	// setup the real Graphic Context
	gc = XCreateGC(dpy, win, valuemask, &values);
	XSetForeground(dpy, gc, white);
	XSetBackground(dpy, gc, black);
	keve = (XKeyEvent *)&event;

	// setup the erase rectangle Graphic Context
	rectgc = XCreateGC(dpy, win, valuemask, &values);
	XSetForeground(dpy, rectgc, black);
	XSetBackground(dpy, rectgc, black);
	XSetFont(dpy, gc, font->fid);

	// specify accepted XEvent loop events
	XSelectInput(dpy, win,
		KeyPressMask|\
		FocusChangeMask|\
		StructureNotifyMask|\
		ButtonPressMask|\
		ButtonReleaseMask|\
		ExposureMask|\
		PropertyChangeMask|\
		Button1MotionMask|\
		Button2MotionMask|\
		Button3MotionMask|\
		VisibilityChangeMask
	);
}

void redraw()
{
	int i=0, j=0;
	char status[80];
	char* p = curp;
	int l = 1; 

	if(curp < page) page = prevline(curp);
	if(epage <= curp) {
		page = prevline(curp);
		i = screen_height-1;
		while (--i) page = prevline(page-1);
	}
	epage = page;
	if(mk) {
		if(curp < mk) { bstart = curp; bend = mk; }
		else { bstart = mk; bend = curp; }
	} else bstart = bend = NULL;
	// draw the black on black erase rectangle
	XFillRectangle(dpy, win, rectgc, 0, 0, winwidth, winheight);
	gotoxy(0,1);
	while (i <= screen_height-2) {
		if(curp == epage) { row = i; col = j; }
		if(*epage == '\n') { ++i; j = 0; }
		if(line_count <= i || etxt <= epage) break;
		if(*epage != '\r') {
			if((epage >= bstart) && (epage < bend)) highvideo();
			if(epage == bend) lowvideo();
			if(screen_width > j) emitch(*epage);
			j += *epage == '\t' ? TABSZ-(j & (TABM)) : *epage == '\n' ? 0 : 1;
		}
		++epage;
	}
	lowvideo();
	i = xy.Y;
	while(i++ < screen_height-2) { clrtoeol(); gotoxy(0,i); }
	// draw status line
	highvideo();
	while(p > buf) { if(*--p == '\n') l++; };
	gotoxy(0,0);
	clrtoeol();
	sprintf(status, "  %6d %5d %c %04x F1-Help", l, col+1,
			((void*)undop > (void*)ubuf) ? '*' : ' ', ch&0xffff);
	addstr((void*)&status);
	lowvideo();
	gotoxy(col, row+1);
	draw_cursor();
	XFlush(dpy);
}

int main(int argc, char **argv)
{
	int i=0, j=1;
	// error on no file name
	if (argc < 2) {
		printf("usage: %s <filename>[:line#] \n",argv[0]);return (1);
	}
	init();
	filename = *++argv;
	{
		char* p = strchr(*argv, ':');
		if(p) { *p++ = '\0'; j = atoi(p); }
	}
	set_title(filename);
	if(0 < (i = open(filename, 0))) {
		etxt += read(i, buf, BUF);
		if(etxt < buf) etxt = buf;
		else{
			char *p = etxt;
			while(p > buf) if(*--p == '\n') line_count++;
		}
		close(i);
	}
	goln(j);
	while (!done) {
		redraw();
		while ((ch = getch()) == -1);
		// enable marked block with Shift
		if((event.xkey.state & ShiftMask) && !mk && 
			((ch >= XK_Home && ch <= XK_End) ||
				(ch >= (XK_Home & 0x7fff) && ch <= (XK_End & 0x7fff))))
				block_mark();
		i = 0; 
		while (key[i] != ch && key[i] != '\0') ++i;
		if(key[i] == '\0') {
			handle_uekey(ch);
		} else {
			if((ch != 0xffff) && (ch != 0xff08)) oblk();
			(*func[i])();
			cur_pos = curp;
		}
	}
	return (0);
}
