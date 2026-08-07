/*
 * clil - minimal streaming CLI client for Ollama (raw sockets, zero deps)
 *
 *   clil "prompt"                                   use default model
 *   clil -m qwen2.5-coder:14b "write a sort in C"
 *   clil -s "You are a coding assistant" -t 0.2 "refactor this"
 *   echo "code" | clil "explain this"
 *   OLLAMA_HOST=192.168.1.5:11434 clil "hello"
 *
 * Env: OLLAMA_HOST (default localhost:11434), OLLAMA_MODEL (default codellama)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

static char *host = "localhost";
static char *port = "11434";
static char *model = "codellama";
static const char *system_prompt = NULL;
static double temp = 0.0;
static int have_temp = 0;

static void die(const char *m){ fprintf(stderr,"clil: %s\n", m); exit(1); }

static char *read_stdin(void){
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if(!buf) die("out of memory");
    for(;;){
        if(len == cap){ cap *= 2; char *n = realloc(buf, cap); if(!n) die("out of memory"); buf = n; }
        size_t r = fread(buf+len, 1, cap-len, stdin);
        if(!r) break;
        len += r;
    }
    buf[len] = 0;
    return buf;
}

static char *jesc(const char *s){
    size_t n = 2;
    const unsigned char *p;
    for(p=(const unsigned char*)s; *p; p++){
        if(*p=='"'||*p=='\\') n += 2;
        else if(*p<0x20 || *p==0x7f) n += 6;
        else n++;
    }
    char *out = malloc(n+1), *q = out;
    *q++ = '"';
    for(p=(const unsigned char*)s; *p; p++){
        if(*p=='"'||*p=='\\'){ *q++='\\'; *q++=*p; }
        else if(*p<0x20 || *p==0x7f){ q += sprintf(q,"\\u%04x",(unsigned)*p); }
        else *q++ = *p;
    }
    *q++ = '"'; *q = 0;
    return out;
}

static int connect_ollama(void){
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo(host, port, &hints, &res);
    if(g){ fprintf(stderr,"clil: resolve %s: %s\n", host, gai_strerror(g)); return -1; }
    int fd = -1;
    for(rp = res; rp; rp = rp->ai_next){
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if(fd < 0) continue;
        if(connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int http_post(int fd, const char *body){
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "POST /api/generate HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        host, port, strlen(body));
    if(write(fd, hdr, n) != n) return -1;
    return write(fd, body, strlen(body)) == (long)strlen(body) ? 0 : -1;
}

static void print_unescaped(const char *s, size_t len){
    const unsigned char *end = (const unsigned char*)s + len;
    for(const unsigned char *c = (const unsigned char*)s; c < end; c++){
        if(*c != '\\'){ putchar(*c); continue; }
        c++;
        if(c >= end) break;
        switch(*c){
            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case 'r': putchar('\r'); break;
            case 'b': putchar('\b'); break;
            case '\\': putchar('\\'); break;
            case '"': putchar('"'); break;
            case '/': putchar('/'); break;
            case 'u': { /* \uXXXX */
                unsigned v = 0;
                for(int i=0;i<4 && c+1<end;i++){ c++; v = v*16 + ("0123456789abcdef0123456789ABCDEF"[(strchr("0123456789abcdefABCDEF", *c))?strchr("0123456789abcdefABCDEF",*c)-"0123456789abcdefABCDEF":0]&0xf); }
                putchar(v > 127 ? '?' : v);
                break;
            }
            default: putchar(*c);
        }
    }
}

/* streaming via pointer arithmetic over a FIXED smal buffer (no heap growth).
 * Each NDJSON object is one line; decode the "response" field in place. */
static int stream(int fd){
    char buf[2048];               /* stack only, no malloc */
    size_t used = 0;
    int bodies = 0;               /* have we seen \r\n\r\n yet (skips headers) */
    static const char MARK[] = "\"response\":\"";

    for(;;){
        ssize_t r = read(fd, buf + used, sizeof buf - 1 - used);
        if(r == 0) break;
        if(r < 0){ if(errno == EINTR) continue; break; }
        used += (size_t)r;
        buf[used] = 0;

        if(!bodies){
            char *hd = strstr(buf, "\r\n\r\n");
            if(hd){ bodies = 1;
                    memmove(buf, hd+4, used - (size_t)(hd - buf) - 4);
                    used = strlen(buf);
            }
            if(!bodies || used == 0) continue;
        }

        /* scan the current chunk with pointers only */
        char *p = buf;
        while((p = strstr(p, MARK))){
            char *s = p + strlen(MARK);         /* start of the string value   */
            char *e = s;                         /* find its closing quote      */
            while(*e && *e != '"') e++;          /* stop at quote or NUL       */
            if(!*e) break;                       /* value not complete yet      */
            print_unescaped(s, (size_t)(e - s)); /* emit token now (live)       */
            fflush(stdout);
            p = e + 1;                           /* advance past closing quote  */
        }

        /* keep only the incomplete tail (bytes after last '\n') */
        char *nl = buf + used;
        while(nl > buf && nl[-1] != '\n') nl--;
        size_t keep = (size_t)(buf + used - nl);
        memmove(buf, nl, keep);
        used = keep;
        buf[used] = 0;
    }
    putchar('\n');
    return 0;
}

int main(int argc, char **argv){
    const char *prompt = NULL;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i],"-m") && i+1<argc) model = argv[++i];
        else if(!strcmp(argv[i],"-H") && i+1<argc){ char *c=strchr(argv[++i],':'); if(c){ *c=0; host=argv[i]; port=c+1; } else host=argv[i]; }
        else if(!strcmp(argv[i],"-s") && i+1<argc) system_prompt = argv[++i];
        else if(!strcmp(argv[i],"-t") && i+1<argc){ temp = atof(argv[++i]); have_temp = 1; }
        else if(!strcmp(argv[i],"-h")){ fprintf(stderr,"usage: clil [-m model] [-s system] [-t temp] [-H host:port] [prompt]\n"); return 0; }
        else if(argv[i][0]=='-') die("unknown flag (see -h)");
        else prompt = argv[i];
    }
    const char *eh = getenv("OLLAMA_HOST");
    if(eh){ char *c=strchr(eh,':'); if(c){ *c=0; host=(char*)eh; port=c+1; } else host=(char*)eh; }
    const char *em = getenv("OLLAMA_MODEL");
    if(em) model = (char*)em;

    if(!prompt) prompt = read_stdin();
    char *pj = jesc(prompt);
    char *sj = system_prompt ? jesc(system_prompt) : NULL;

    char *body = malloc(256 + strlen(model) + strlen(pj) + (sj?strlen(sj)+64:0) + 64);
    char *o = body;
    o += sprintf(o, "{\"model\":%s,\"prompt\":%s,\"stream\":true,\"keep_alive\":\"0\"", jesc(model), pj);
    if(sj) o += sprintf(o, ",\"system\":%s", sj);
    if(have_temp) o += sprintf(o, ",\"temperature\":%.3f", temp);
    sprintf(o, "}");

    int fd = connect_ollama();
    if(fd < 0) die("cannot connect to ollama");
    if(http_post(fd, body)){ close(fd); free(body); free(pj); free(sj); die("send failed"); }
    free(body); free(pj); free(sj);

    stream(fd);
    close(fd);
    return 0;
}
