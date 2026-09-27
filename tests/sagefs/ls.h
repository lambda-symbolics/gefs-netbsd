/* libsa's ls.h, for sagefs(1) */
typedef struct lsentry {
	struct lsentry	*e_next;
	uint32_t	e_ino;
	const char	*e_type;
	char		e_name[1];
} lsentry_t;

void	lsadd(lsentry_t **, const char *, const char *, size_t, uint32_t, const char *);
void	lsprint(lsentry_t *);
void	lsfree(lsentry_t *);
