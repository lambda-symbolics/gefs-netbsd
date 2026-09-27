/*
 * A small implementation of Plan 9's print(2) and fmtinstall(2), enough for
 * the gefs core, for the kernel and for userland: the flags - + space # 0 ,
 * and the u, h, l, ll modifiers; width and precision (also as *); the verbs
 * d i o x X b c s q p r % and the ones installed with fmtinstall.
 */
#include <u.h>
#include <libc.h>

static int	(*fmtfns[128])(Fmt*);

int
fmtinstall(int c, int (*f)(Fmt*))
{
	if(c <= 0 || c >= (int)nelem(fmtfns))
		return -1;
	fmtfns[c] = f;
	return 0;
}

static void
fmtputc(Fmt *f, int c)
{
	if(f->to < f->stop)
		*f->to++ = c;
	f->nfmt++;
}

static void
fmtpad(Fmt *f, int n, int c)
{
	while(n-- > 0)
		fmtputc(f, c);
}

static void
fmtputs(Fmt *f, const char *s, int n)
{
	while(n-- > 0)
		fmtputc(f, *s++);
}

int
fmtstrcpy(Fmt *f, char *s)
{
	int n;

	if(s == nil)
		s = "<nil>";
	n = strlen(s);
	fmtputs(f, s, n);
	return 0;
}

static void
fmtstr(Fmt *f, const char *s)
{
	int n, w;

	if(s == nil)
		s = "<nil>";
	for(n = 0; s[n] != 0; n++)
		if((f->flags & FmtPrec) && n >= f->prec)
			break;
	w = (f->flags & FmtWidth) ? f->width - n : 0;
	if(!(f->flags & FmtLeft))
		fmtpad(f, w, ' ');
	fmtputs(f, s, n);
	if(f->flags & FmtLeft)
		fmtpad(f, w, ' ');
}

static void
fmtint(Fmt *f, uvlong u, int neg, int base, int upper)
{
	char buf[80], *p, *digits;
	int n, w, z, ndig, sign;

	digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	p = buf + sizeof(buf);
	ndig = 0;
	do{
		if((f->flags & FmtComma) && ndig > 0 && ndig % 3 == 0)
			*--p = ',';
		*--p = digits[u % base];
		u /= base;
		ndig++;
	}while(u != 0);
	n = buf + sizeof(buf) - p;
	z = 0;
	if((f->flags & FmtPrec) && f->prec > ndig)
		z = f->prec - ndig;
	sign = 0;
	if(neg)
		sign = '-';
	else if(f->flags & FmtSign)
		sign = '+';
	else if(f->flags & FmtSpace)
		sign = ' ';
	w = n + z + (sign != 0);
	if((f->flags & FmtSharp) && base == 16)
		w += 2;
	else if((f->flags & FmtSharp) && base == 8 && z == 0)
		w += 1;
	w = (f->flags & FmtWidth) ? f->width - w : 0;
	if(w > 0 && (f->flags & FmtZero) && !(f->flags & (FmtLeft|FmtPrec))){
		z += w;
		w = 0;
	}
	if(!(f->flags & FmtLeft))
		fmtpad(f, w, ' ');
	if(sign)
		fmtputc(f, sign);
	if((f->flags & FmtSharp) && base == 16){
		fmtputc(f, '0');
		fmtputc(f, upper ? 'X' : 'x');
	}else if((f->flags & FmtSharp) && base == 8 && z == 0)
		fmtputc(f, '0');
	fmtpad(f, z, '0');
	fmtputs(f, p, n);
	if(f->flags & FmtLeft)
		fmtpad(f, w, ' ');
}

static void
fmtnum(Fmt *f, int base, int upper, int issigned)
{
	uvlong u;
	vlong v;
	int neg;

	if(f->flags & FmtUnsigned)
		issigned = 0;
	neg = 0;
	if(f->flags & FmtVLong){
		if(issigned){
			v = va_arg(f->args, vlong);
			neg = v < 0;
			u = neg ? -(uvlong)v : (uvlong)v;
		}else
			u = va_arg(f->args, uvlong);
	}else if(f->flags & FmtLong){
		if(issigned){
			v = va_arg(f->args, long);
			neg = v < 0;
			u = neg ? -(uvlong)v : (uvlong)v;
		}else
			u = va_arg(f->args, ulong);
	}else{
		if(issigned){
			v = va_arg(f->args, int);
			if(f->flags & FmtByte)
				v = (schar)v;
			else if(f->flags & FmtShort)
				v = (short)v;
			neg = v < 0;
			u = neg ? -(uvlong)v : (uvlong)v;
		}else{
			u = va_arg(f->args, uint);
			if(f->flags & FmtByte)
				u = (uchar)u;
			else if(f->flags & FmtShort)
				u = (ushort)u;
		}
	}
	fmtint(f, u, neg, base, upper);
}

static int
dofmt(Fmt *f, char *fmt)
{
	int c, n;

	while((c = *fmt++) != 0){
		if(c != '%'){
			fmtputc(f, c);
			continue;
		}
		f->flags = 0;
		f->width = 0;
		f->prec = 0;
		for(;;){
			c = *fmt++;
			switch(c){
			case '-': f->flags |= FmtLeft; continue;
			case '+': f->flags |= FmtSign; continue;
			case ' ': f->flags |= FmtSpace; continue;
			case '#': f->flags |= FmtSharp; continue;
			case ',': f->flags |= FmtComma; continue;
			case 'u': f->flags |= FmtUnsigned; continue;
			case 'h':
				if(f->flags & FmtShort)
					f->flags |= FmtByte;
				f->flags |= FmtShort;
				continue;
			case 'l':
				if(f->flags & FmtLong)
					f->flags |= FmtVLong;
				f->flags |= FmtLong;
				continue;
			case '0':
				if(!(f->flags & (FmtWidth|FmtPrec))){
					f->flags |= FmtZero;
					continue;
				}
				/* fall through: part of a number */
			case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				n = c - '0';
				while(*fmt >= '0' && *fmt <= '9')
					n = n*10 + (*fmt++ - '0');
				if(f->flags & FmtPrec)
					f->prec = n;
				else{
					f->flags |= FmtWidth;
					f->width = n;
				}
				continue;
			case '.':
				f->flags |= FmtPrec;
				continue;
			case '*':
				n = va_arg(f->args, int);
				if(f->flags & FmtPrec)
					f->prec = n;
				else{
					f->flags |= FmtWidth;
					if(n < 0){
						f->flags |= FmtLeft;
						n = -n;
					}
					f->width = n;
				}
				continue;
			}
			break;
		}
		f->r = c;
		switch(c){
		case 0:
			return f->nfmt;
		case '%':
			fmtputc(f, '%');
			break;
		case 'd': case 'i':
			fmtnum(f, 10, 0, 1);
			break;
		case 'o':
			fmtnum(f, 8, 0, 0);
			break;
		case 'x':
			fmtnum(f, 16, 0, 0);
			break;
		case 'X':
			fmtnum(f, 16, 1, 0);
			break;
		case 'b':
			fmtnum(f, 2, 0, 0);
			break;
		case 'p':
			f->flags |= FmtVLong;
			fmtint(f, (uvlong)(uintptr)va_arg(f->args, void*), 0, 16, 0);
			break;
		case 'c':
			fmtputc(f, va_arg(f->args, int));
			break;
		case 's': case 'q':
			fmtstr(f, va_arg(f->args, char*));
			break;
		case 'r':
			fmtstr(f, p9errstr());
			break;
		default:
			if(c > 0 && c < (int)nelem(fmtfns) && fmtfns[c] != nil)
				(*fmtfns[c])(f);
			else{
				fmtputc(f, '%');
				fmtputc(f, c);
			}
			break;
		}
	}
	return f->nfmt;
}

int
fmtprint(Fmt *f, char *fmt, ...)
{
	va_list saved;
	ulong flags;
	int width, prec, r, n0;

	flags = f->flags;
	width = f->width;
	prec = f->prec;
	r = f->r;
	n0 = f->nfmt;
	va_copy(saved, f->args);
	va_end(f->args);
	va_start(f->args, fmt);
	dofmt(f, fmt);
	va_end(f->args);
	va_copy(f->args, saved);
	va_end(saved);
	f->flags = flags;
	f->width = width;
	f->prec = prec;
	f->r = r;
	return f->nfmt - n0;
}

char*
vseprint(char *buf, char *e, char *fmt, va_list args)
{
	Fmt f;

	if(e <= buf)
		return buf;
	f.start = buf;
	f.to = buf;
	f.stop = e - 1;
	f.nfmt = 0;
	va_copy(f.args, args);
	dofmt(&f, fmt);
	va_end(f.args);
	*f.to = 0;
	return f.to;
}

char*
seprint(char *buf, char *e, char *fmt, ...)
{
	va_list args;
	char *p;

	va_start(args, fmt);
	p = vseprint(buf, e, fmt, args);
	va_end(args);
	return p;
}

int
vsnprint(char *buf, int len, char *fmt, va_list args)
{
	if(len <= 0)
		return 0;
	return vseprint(buf, buf + len, fmt, args) - buf;
}

int
snprint(char *buf, int len, char *fmt, ...)
{
	va_list args;
	int n;

	va_start(args, fmt);
	n = vsnprint(buf, len, fmt, args);
	va_end(args);
	return n;
}

/* %.*H: the precision is the number of bytes to print in hex */
int
encodefmt(Fmt *f)
{
	uchar *p;
	int i, n;

	p = va_arg(f->args, uchar*);
	n = (f->flags & FmtPrec) ? f->prec : 0;
	if(p == nil)
		return fmtstrcpy(f, "<nil>");
	for(i = 0; i < n; i++){
		fmtputc(f, "0123456789abcdef"[p[i] >> 4]);
		fmtputc(f, "0123456789abcdef"[p[i] & 15]);
	}
	return 0;
}
