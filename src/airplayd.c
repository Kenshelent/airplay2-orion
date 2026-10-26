/* airplayd (Yandex Station Mini 3 / bergamot) - buttons, volume sync, audio-reactive ring + display for shairport-sync.
 *   ring     : /dev/fb0 is a 6x1 24bpp framebuffer (BGR byte order) driving the 6 RGB "logo" LEDs.
 *   display  : 4 digits x 7 segments (+ colon, degree) as plain sysfs LEDs bergamot:white:clock<A-G><0-3>.
 *   audio    : shairport plays through ALSA pcm "spk_tee" which copies the stream into /tmp/sps.fifo;
 *              we measure its level and use it for the ring (rainbow ripple) and the display (4-bar equaliser).
 *   volume   : phone slider -> shairport metadata "pvol" -> ALSA Master. Buttons change Master too.
 *   buttons  : evdev event1 (touch): 114 VOL-, 115 VOL+, 582 centre. Taps: vol -/+, play/pause.
 *              A swipe over the pads (two or more pads within 180 ms) = next / previous track.
 *   idle     : the display shows the time (MSK).
 * Needs quasar dead (it owns the LEDs/buttons otherwise).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/input.h>

#define FIFO   "/tmp/sps.fifo"
#define META   "/tmp/shairport-sync-ap2-metadata"
#define NLED   6
#define FRAME  2400            /* samples per level frame = 50 ms */
#define HIST   64
#define K_VOLDOWN 114
#define K_VOLUP   115
#define K_VOICE   582

static int delay_frames = 5;   /* light lags the audio so it matches the (buffered) audible output */

static long now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }
static void run(const char *c){ if (system(c)) {} }

/* ---- volume ---- */
static int vol_shown = -1; static long vol_until;      /* overlay on ring + display */

static int get_master(void){
  FILE *p = popen("amixer -c0 sget Master 2>/dev/null", "r");
  int v = -1; char l[200];
  if (p){
    while (fgets(l, sizeof l, p)){ char *b = strchr(l, '['); if (b && strchr(b, '%')) v = atoi(b + 1); }
    pclose(p);
  }
  return v;
}

static void show_volume(int pct){ vol_shown = pct; vol_until = now_ms() + 1500; }

static void set_master(int pct){
  char c[96];
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  snprintf(c, sizeof c, "amixer -c0 sset Master %d%% >/dev/null 2>&1", pct);
  run(c);
  show_volume(pct);
}

static void master_step(int d){
  char c[96];
  snprintf(c, sizeof c, "amixer -c0 sset Master 5%%%c >/dev/null 2>&1", d > 0 ? '+' : '-');
  run(c);
  int v = get_master();
  if (v >= 0) show_volume(v);
}

/* ---- ring ---- */
static int fbfd = -1;
static unsigned char last_px[NLED * 3];
static int px_valid;

/* The ring is driven through sysfs: bergamot:{red,green,blue}:logo0..5/brightness (the framebuffer path is
 * gated by the kernel's ledfb trigger and did not light anything here). pixels are B,G,R per LED. */
static int ring_fd[NLED][3];                           /* [led][0]=blue [1]=green [2]=red */
static int ring_ready;

static void ring_open(void){
  static const char *cn[3] = {"blue", "green", "red"};
  char p[96];
  for (int i = 0; i < NLED; i++) for (int c = 0; c < 3; c++){
    int fd;
    snprintf(p, sizeof p, "/sys/class/leds/bergamot:%s:logo%d/trigger", cn[c], i);
    fd = open(p, O_WRONLY); if (fd >= 0){ if (write(fd, "none", 4) < 0) {} close(fd); }
    snprintf(p, sizeof p, "/sys/class/leds/bergamot:%s:logo%d/brightness", cn[c], i);
    ring_fd[i][c] = open(p, O_WRONLY);
  }
  ring_ready = 1;
}

static void fb_write(const unsigned char *bgr){
  char b[8]; int n;
  if (px_valid && !memcmp(last_px, bgr, sizeof last_px)) return;
  if (!ring_ready) ring_open();
  for (int i = 0; i < NLED; i++) for (int c = 0; c < 3; c++){
    if (px_valid && last_px[i * 3 + c] == bgr[i * 3 + c]) continue;
    if (ring_fd[i][c] < 0) continue;
    n = snprintf(b, sizeof b, "%d\n", bgr[i * 3 + c]);
    if (pwrite(ring_fd[i][c], b, n, 0) < 0) {}
  }
  memcpy(last_px, bgr, sizeof last_px); px_valid = 1;
}

static void hsv(float h, float v, unsigned char *b, unsigned char *g, unsigned char *r){
  float c = v, hh = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
  float x = c * (1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f)), rr = 0, gg = 0, bb = 0;
  if (hh < 1){ rr = c; gg = x; } else if (hh < 2){ rr = x; gg = c; }
  else if (hh < 3){ gg = c; bb = x; } else if (hh < 4){ gg = x; bb = c; }
  else if (hh < 5){ rr = x; bb = c; } else { rr = c; bb = x; }
  *r = (unsigned char)(rr * 255); *g = (unsigned char)(gg * 255); *b = (unsigned char)(bb * 255);
}

/* ---- display ---- */
static int seg_fd[7][4], colon_fd[4];                  /* colon_fd: DH, DL, DM, DEGREE */
static int seg_last[7][4], colon_last[4];
static const unsigned char DIGIT[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F}; /* bit0=A .. bit6=G */

static void clk_open(void){
  char p[96];
  for (int s = 0; s < 7; s++) for (int d = 0; d < 4; d++){
    snprintf(p, sizeof p, "/sys/class/leds/bergamot:white:clock%c%d/brightness", 'A' + s, d);
    seg_fd[s][d] = open(p, O_WRONLY); seg_last[s][d] = -1;
  }
  static const char *cn[4] = {"DH", "DL", "DM", "DEGREE"};
  for (int i = 0; i < 4; i++){
    snprintf(p, sizeof p, "/sys/class/leds/bergamot:white:clock%s/brightness", cn[i]);
    colon_fd[i] = open(p, O_WRONLY); colon_last[i] = -1;
  }
}

static void put(int fd, int *last, int v){
  char b[8]; int n;
  if (v < 0) v = 0; if (v > 255) v = 255;
  if (*last == v || fd < 0) return;
  n = snprintf(b, sizeof b, "%d\n", v);
  if (pwrite(fd, b, n, 0) == n) *last = v;
}
static void seg(int s, int d, int v){ put(seg_fd[s][d], &seg_last[s][d], v); }
static void colon(int on_hl, int v){
  put(colon_fd[0], &colon_last[0], on_hl ? v : 0); put(colon_fd[1], &colon_last[1], on_hl ? v : 0);
  put(colon_fd[2], &colon_last[2], 0); put(colon_fd[3], &colon_last[3], 0);   /* DM, DEGREE stay off */
}
static void digit(int d, int ch, int v){                /* ch 0..9, or -1 = blank */
  unsigned m = ch >= 0 && ch <= 9 ? DIGIT[ch] : 0;
  for (int s = 0; s < 7; s++) seg(s, d, (m >> s) & 1 ? v : 0);
}

/* ---- audio level history ---- */
static float hist[HIST];
static unsigned hpos;
static double sumsq; static int nsmp;
static float smooth, prev_lv, avg_db = -25.0f;
static time_t last_audio;
static float hue = 200.0f;

static void push_level(float lv){
  /* fast attack, slow release so beats read but it doesn't flicker */
  smooth = lv > smooth ? lv : smooth * 0.8f + lv * 0.2f;
  if (lv > prev_lv + 0.25f) hue += 28.0f;               /* kick the colour on a beat */
  prev_lv = lv;
  hist[hpos++ % HIST] = smooth;
}

static float lvl(int back){ return hist[(((int)hpos - 1 - back) % HIST + HIST) % HIST]; }

static void feed_audio(const short *s, int frames){
  for (int i = 0; i < frames; i++){
    float m = (s[2 * i] + s[2 * i + 1]) * 0.5f;
    sumsq += (double)m * m;
    if (++nsmp >= FRAME){
      float rms = (float)sqrt(sumsq / nsmp) / 32768.0f;
      float db = rms > 1e-5f ? 20.0f * log10f(rms) : -100.0f;
      /* relative to the recent average, so any loudness fills the whole range: avg -> 0.5, +-5.5 dB -> 0..1 */
      float lv = 0;
      if (db > -58.0f){
        avg_db += (db - avg_db) * 0.03f;
        lv = 0.5f + (db - avg_db) / 11.0f;
      }
      push_level(lv < 0 ? 0 : lv > 1 ? 1 : lv);
      sumsq = 0; nsmp = 0;
    }
  }
  last_audio = time(NULL);
}

static void render(int active){
  unsigned char px[NLED * 3];
  long t = now_ms();
  int overlay = t < vol_until;
  memset(px, 0, sizeof px);

  /* ring */
  if (overlay){                                         /* volume bar: green -> red */
    for (int i = 0; i < NLED; i++){
      float f = vol_shown / 100.0f * NLED - i; f = f < 0 ? 0 : f > 1 ? 1 : f;
      hsv(120.0f - 120.0f * vol_shown / 100.0f, f * 0.8f, &px[i * 3], &px[i * 3 + 1], &px[i * 3 + 2]);
    }
  } else if (active){                                   /* rainbow ripple, brightness follows the music */
    hue += 0.8f;
    for (int i = 0; i < NLED; i++){
      float lv = lvl(delay_frames + i);
      float v = 0.06f + powf(lv, 1.4f) * 0.85f;
      hsv(hue + i * 60.0f, v, &px[i * 3], &px[i * 3 + 1], &px[i * 3 + 2]);
    }
  }
  fb_write(px);

  /* display */
  if (overlay){                                         /* volume number, right aligned */
    int v = vol_shown;
    digit(0, -1, 0);
    digit(1, v >= 100 ? 1 : -1, 220);
    digit(2, v >= 100 ? (v / 10) % 10 : (v >= 10 ? v / 10 : -1), 220);
    digit(3, v % 10, 220);
    colon(0, 0);
  } else if (active){                                   /* Alice-style wave: the middle bar (G) glows with the music */
    static const int lag[4] = {3, 0, 0, 3};             /* middle digits lead, the outer ones follow -> ripple outwards */
    for (int d = 0; d < 4; d++){
      float lv = lvl(delay_frames + lag[d]);
      float peak = (lv - 0.45f) / 0.55f; peak = peak < 0 ? 0 : peak > 1 ? 1 : peak;
      seg(6, d, 6 + (int)(lv * 249));                   /* G: always a dim line, brighter with level */
      seg(0, d, (int)(peak * peak * 255));              /* A, D: only on loud passages */
      seg(3, d, (int)(peak * peak * 255));
      seg(1, d, 0); seg(2, d, 0); seg(4, d, 0); seg(5, d, 0);
    }
    colon(0, 0);
  } else {                                              /* clock */
    time_t tt = time(NULL); struct tm tmv; localtime_r(&tt, &tmv);
    int h = tmv.tm_hour, m = tmv.tm_min;
    digit(0, h >= 10 ? h / 10 : -1, 70); digit(1, h % 10, 70);
    digit(2, m / 10, 70); digit(3, m % 10, 70);
    colon(1, (tmv.tm_sec & 1) ? 0 : 70);
  }
}

/* ---- metadata: pvol -> Master ---- */
static int b64v(int c){
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}

static void handle_pvol(const char *b64){
  char out[96]; int n = 0, acc = 0, bits = 0;
  for (const char *p = b64; *p && n < 90; p++){
    int v = b64v(*p); if (v < 0) continue;
    acc = (acc << 6) | v; bits += 6;
    if (bits >= 8){ bits -= 8; out[n++] = (acc >> bits) & 0xff; }
  }
  out[n] = 0;
  float vol = strtof(out, NULL);                      /* airplay volume: -30..0 dB, -144 = mute */
  int pct = vol <= -100.0f ? 0 : (int)(100.0f * powf(10.0f, vol / 40.0f) + 0.5f);
  set_master(pct);
}

static char mbuf[8192]; static int mlen;

static void meta_feed(const char *d, int n){
  if (mlen + n >= (int)sizeof mbuf) mlen = 0;
  memcpy(mbuf + mlen, d, n); mlen += n; mbuf[mlen] = 0;
  for (;;){
    char *s = strstr(mbuf, "<item>"), *e = s ? strstr(s, "</item>") : NULL;
    if (!e){ if (!s) mlen = 0; return; }
    e += 7;
    if (strstr(s, "<type>73736e63</type>") && strstr(s, "<code>70766f6c</code>")){
      char *dp = strstr(s, "<data encoding=\"base64\">");
      if (dp && dp < e){ dp += 24; char *q = strstr(dp, "</data>"); if (q){ *q = 0; handle_pvol(dp); } }
    }
    memmove(mbuf, e, mlen - (e - mbuf) + 1); mlen -= e - mbuf;
  }
}

static int open_ro(const char *p){ return open(p, O_RDONLY | O_NONBLOCK); }

/* ---- touch pads: taps vs swipes ----
 * A swipe sweeps all three pads in a row (e.g. 115 -> 582 -> 114), so presses are collected for 180 ms
 * and then classified: one pad = tap, two or more pads = swipe, direction from the first volume pad hit. */
static int pend_key[8], pend_n; static long pend_last;

/* transport commands go to shairport's event-channel thread (UDP 127.0.0.1:7001), which relays them to the phone */
static void send_cmd(const char *name){
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return;
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET; a.sin_port = htons(7001); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (sendto(s, name, strlen(name), 0, (struct sockaddr *)&a, sizeof a) < 0) {}
  close(s);
}

static void flash(unsigned char b, unsigned char g, unsigned char r){
  unsigned char px[NLED * 3];
  for (int i = 0; i < NLED; i++){ px[i * 3] = b; px[i * 3 + 1] = g; px[i * 3 + 2] = r; }
  fb_write(px);
}

static void flush_keys(void){
  int has[3] = {0, 0, 0}, first = 0;                  /* [0]=114 [1]=115 [2]=582 */
  for (int i = 0; i < pend_n; i++){
    int k = pend_key[i];
    if (k == K_VOLDOWN) has[0] = 1; else if (k == K_VOLUP) has[1] = 1; else if (k == K_VOICE) has[2] = 1;
    if (!first && (k == K_VOLDOWN || k == K_VOLUP)) first = k;
  }
  pend_n = 0;
  if (has[0] + has[1] + has[2] >= 2){
    /* swipe: 114 first = towards 115 side ("next"), 115 first = "prev" */
    int fwd = first == K_VOLDOWN;
    flash(fwd ? 255 : 0, fwd ? 120 : 80, fwd ? 0 : 255);
    send_cmd(fwd ? "next" : "previous");
    usleep(150000);
    px_valid = 0;
    return;
  }
  if (has[1]) master_step(1);
  else if (has[0]) master_step(-1);
  else if (has[2]){ send_cmd("playpause"); flash(255, 255, 255); usleep(80000); px_valid = 0; }
}

int main(int argc, char **argv){
  if (argc > 2 && !strcmp(argv[1], "-d")) delay_frames = atoi(argv[2]);
  if (!getenv("TZ")) setenv("TZ", "MSK-3", 1);
  tzset();
  mkfifo(FIFO, 0666);
  int ffd = open(FIFO, O_RDWR | O_NONBLOCK);          /* O_RDWR: never EOF, and the ALSA writer never blocks on open */
  int mfd = open_ro(META);
  int kfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
  time_t last_meta_try = 0;
  unsigned char abuf[16384]; int aleft = 0;
  clk_open();
  render(0);

  for (;;){
    struct pollfd p[3]; int np = 0, ia = -1, im = -1, ik = -1;
    if (ffd >= 0){ p[np].fd = ffd; p[np].events = POLLIN; ia = np++; }
    if (mfd >= 0){ p[np].fd = mfd; p[np].events = POLLIN; im = np++; }
    if (kfd >= 0){ p[np].fd = kfd; p[np].events = POLLIN; ik = np++; }
    int r = poll(p, np, 50);
    if (r < 0 && errno != EINTR) break;
    if (r > 0){
      for (int k = 0; k < np; k++){
        if (!(p[k].revents & (POLLIN | POLLHUP))) continue;
        if (k == ia){
          ssize_t n;
          while ((n = read(ffd, abuf + aleft, sizeof abuf - aleft)) > 0){
            int tot = aleft + (int)n, frames = tot / 4;
            feed_audio((short *)abuf, frames);
            aleft = tot - frames * 4;
            memmove(abuf, abuf + frames * 4, aleft);
          }
        } else if (k == im){
          char t[2048]; ssize_t n = read(mfd, t, sizeof t);
          if (n > 0) meta_feed(t, (int)n);
          else if (n == 0 || (n < 0 && errno != EAGAIN)){ close(mfd); mfd = -1; }
        } else if (k == ik){
          struct input_event ev;
          while (read(kfd, &ev, sizeof ev) == (int)sizeof ev){
            if (ev.type != EV_KEY || ev.value != 1) continue;
            if (pend_n < 8) pend_key[pend_n++] = ev.code;
            pend_last = now_ms();
          }
        }
      }
    }
    if (pend_n && now_ms() - pend_last > 180) flush_keys();
    time_t now = time(NULL);
    if (mfd < 0 && now != last_meta_try){ last_meta_try = now; mfd = open_ro(META); }
    int active = (now - last_audio) < 2;
    if (!active || r == 0) push_level(0);               /* no fresh audio this tick: decay */
    render(active);
  }
  return 0;
}
