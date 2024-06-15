
/* ue is a libc only text editor.
 compile with:
 gcc -g -O2 -fomit-frame-pointer -pipe -Wall -o ue ue.c;sstrip ue;ls -al ue
 Public Domain, (C) 2005 Terry Loveall, 
 THIS PROGRAM COMES WITH ABSOLUTELY NO WARRANTY OR BINARIES. COMPILE AND USE 
 AT YOUR OWN RISK.
 usage: ue <filename> # <filename> NOT optional
*/

#define VERSION "1.38"

#include <termios.h>
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

char helpt[] = 
"\nue ver. 1.38\n"
"left			^S		top of file			^T\n"
"right			^D		bottom of file		^B\n"
"up				^E		del char			^G\n"
"down			^X		del prev char		^H\n"
"word left		^A		del rest of line	^Y\n"
"word right		^F		undo				^U\n"
"line begining	^[		write file			^W\n"
"line end		^]		Look for string		^L\n"
"pgdown			^C		quit				^Q\n"
"pgup			^R		jump to line		^J\n"
"Help		 Alt-H\n"
"\n"
"		Push any key to continue\n"
"\n\n\n\n\n\n\n\n\n";

int line_count = 1;
int done;
int row, col;
char clr;

char ln[MAXCOLS];	// goto line string, used by gotoln()
char ss[MAXCOLS];	// search string, used by look()
char ubuf[BUF];		// undo buffer
char buf[BUF];		// the edit buffer
char *ebuf = buf + BUF;	// end of edit buffer
char *etxt = buf;		// end of edit text
char *curp = buf;		// current position in edit buffer
char *page, *epage;		// start and end of displayed edit text
char *filename;			// file being edited
char ch;				// most recent command/edit character

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

struct termios termios, orig;

// func protos
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
void bksp();
void delrol();
void undo();
void writef();
void look();
void gotoln();
void quit();
void help();
void nop();
void redraw();
int main(int argc, char **argv);

// command key array, '& 0X1F' means control key
char key[22] = { 
'S' & 0X1F, 'X' & 0X1F, 'E' & 0X1F, 'D' & 0X1F,
'A' & 0X1F, 'C' & 0X1F, 'R' & 0X1F, 'F' & 0X1F,
'[' & 0X1F, ']' & 0X1F, 'T' & 0X1F, 'B' & 0X1F,
'G' & 0X1F, 'H' & 0X1F, 'Y' & 0X1F, 'U' & 0X1F,
'W' & 0X1F, 'L' & 0X1F, 'Q' & 0X1F, 'J' & 0X1F,
0XE8, '\0'
};

// command key function array, one to one correspondence to key array, above
void (*func[])() = {
	left, down, up, right, 
	wleft, pgdown, pgup, wright,
	lnbegin, lnend, top, bottom, 
	delete_one, bksp, delrol, undo,
	writef, look, quit, gotoln, 
	help, nop
};

// generic console I/O

void GetSetTerm(int set){
	struct termios *termiop = &orig;
	if(!set) {
		tcgetattr(0,&orig);
		termios = orig;
		termios.c_lflag    &= (~ICANON & ~ECHO & ~ISIG);
		termios.c_iflag    &= (~IXON & ~ICRNL);
		termios.c_cc[VMIN]  = 1;
		termios.c_cc[VTIME] = 0;
		termiop = &termios;
	} //
	tcsetattr(0, TCSANOW, termiop); //
} //
//
void write1(char *s)
{
	write(1, (void*)s, strlen(s));
}

void highvideo()
{
	write1("\033[47m\033[30;1m");	// black char on white background
}

void lowvideo()
{
	write1("\033[0m");	// reset background
}

void gotoxy(int x, int y){
	char str[MAXCOLS];
	sprintf(str,"\033[%03d;%03dH",y,x);
	write1(str);
	xy.Y=y;xy.X=x;
}

char getch()
{
	char ks[6];
	read(0, (void*)&ks, 1);
	if(*ks == 0x1b) { read(0, (void*)&ks[1], 5);if(ks[1] == 0x68)*ks = 0xE8; }
	if(*ks == 0x7f) *ks = '\b';
	return (*ks);
}

void put1(int c)
{
	write1((void*)&c);
	xy.X++;
}

void emitch(int c)
{
	if(c == '\t') do { put1(' '); } while((xy.X-1) & (TABM));
	else put1(c);
	if(c == '\n') { xy.X=1; xy.Y++; };
}

void clrtoeol()
{
	int ox = xy.X, oy = xy.Y;
	while((xy.X-1) < MAXCOLS) put1(' ');
	gotoxy(ox,oy);
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
	if(!row) page = curp+1;
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
	clr++;
}

void pgup()
{
	int i = MAXLINES;
	while (0 < --i) { page = prevline(page-1); up(); }
	clr++;
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
	curp = buf; clr++;
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
		cmove(curp+1, curp, etxt-curp); clr++;
	}
}

void bksp()
{
	if(buf < curp) { left(); delete_one(); }
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
			(int)curp == -(int)curp;		// so insert
			cmove(curp, curp+1, etxt-curp);
			*curp = undop->ch;
			if(*curp == '\n') line_count++;
		}
		else{	// was insert so delete
			if(*curp == '\n') line_count--;
			cmove(curp+1, curp, etxt-curp);
		} clr++;
	}
}

void writef()
{
	int i;
	write(i = creat(filename, MODE), buf, (int)(etxt-buf));
	close(i);
	undop = (U_REC*)&ubuf;
}

int prompt(char *prompt, char *s, char ch)
{
	char c;
	int i = strlen(s);
	gotoxy(17,1);
	clrtoeol();
	write1(prompt);
	write1(s);
	do {
		c = getch();
		if(c == '\b'){
			if(!i) continue;
			i--;emitch(c);emitch(' ');
		}
		else {
			if(i == MAXCOLS) continue;
			s[i++] = c;
		}
		if(c != 0x1b) emitch(c);
	} while(c != 0x1b && c != '\n' && c != '\r' && c != ch );
	s[--i] = 0;
	return(c == 0x1b ? 0 : i);
}

void look()
{
	int i = prompt(" Look for: ", ss, 0x0c);
	if(i){
		do { right(); } while(curp < etxt && scmp(curp,ss,i));
	}
}

void goln(int i)
{
	top(); while(--i > 0) down();
}

void gotoln()
{
	if(prompt(" goto line: ", ln, 0x0a)) goln(atoi(ln));
}

void quit()
{
	if((void*)undop > (void*)ubuf) {
		gotoxy(3,1);
		write1(" File changed. Save? y/n ");
		if(getch() == 'y') writef();
	}
	done = 1;
}

void help()
{
	char *s = helpt;
	gotoxy(1,1);
	while(*s) { emitch(*s); *s++ == '\n' ? clrtoeol() : nop(); }
	getch(); clr++;
}

void nop()
{
}

void redraw()
{
	int i=0, j=0;
	char status[80];
	char* p = curp;
	int l = 1; 

	if(curp < page) { page = prevline(curp); clr++; }
	if(epage <= curp) {
		page = prevline(curp); 
		i = MAXLINES;
		while (--i) page = prevline(page-1);
		clr++;
	}
	if(clr) { printf("\033[H\033[J\n"); clr = 0; }	// clear screen
	epage = page;
	gotoxy(1,2);
	while (1) {
		if(curp == epage) { row = i; col = j; }
		if(*epage == '\n') { ++i; j = 0; }
		if(i >= MAXLINES || line_count <= i || etxt <= epage) break;
		if(*epage != '\r') {
			if(MAXCOLS > j) emitch(*epage);
			j += *epage == '\t' ? TABSZ-(j & (TABM)) : *epage == '\n' ? 0 : 1;
		}
		++epage;
	}
	i = xy.Y;
	while(i <= MAXLINES+1) { clrtoeol(); gotoxy(1,i++); }
	// draw status line
	highvideo();
	while(p > buf) { if(*--p == '\n') l++; };
	gotoxy(1,1);
	clrtoeol();
	sprintf(status, "  %6d %5d %c %02x Alt-H help", l, col+1, \
			((void*)undop > (void*)ubuf) ? '*' : ' ', ch&0xff);
	write1(status);
	lowvideo();

	gotoxy((MAXCOLS > col+1) ? col+1 : MAXCOLS, row+2);
}

int main(int argc, char **argv)
{
	int i=0, j=1;
	// error on no file name
	if (argc < 2) { printf("usage: %s <filename>\n",argv[0]);return (2); }
	GetSetTerm(0);
	filename = *++argv;
	{
		char* p = strchr(*argv, ':');
		if(p) { *p++ = '\0'; j = atoi(p); }
	}
	if(0 < (i = open(filename = *argv, 0))) {
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
		ch = getch(); 
		i = 0; 
		while (key[i] != ch && key[i] != '\0') ++i;
		(*func[i])();
		if((key[i] == '\0') && !(ch & 0x80)) {
			if(etxt < ebuf) {
				cmove(curp, curp+1, etxt-curp);
				*curp = ch == '\r' ? '\n' : ch;
				if(*curp++ == '\n') 
					{ line_count++; clr++; }
				if((char*)&undop[1] < ubuf+BUF){
//					undop->ch = curp[-1];
					undop->pos = (int)curp-1;	// positive means insert
					undop++;
				}
			}
		}
	}
	gotoxy(1,MAXLINES+2);
	GetSetTerm(1);
	return (0);
}
