/* airplayd for Yandex Station 3 (orion): 36-LED ring, volume sync, optional buttons.
 * Mini 3 clock/6-LED code is in src/airplayd.c (bergamot). This build skips the
 * 7-segment clock — Orion has none — and drives orion:{red,green,blue}:ring0..35.
 *
 * Audio copy comes from ALSA pcm "spk_tee" -> /tmp/sps.fifo (same as Mini 3).
 * Phone volume ("pvol") maps onto ALSA Master. Ring is grabbed while music
 * plays and given back to bulbik (ledfb-ring) when idle.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/input.h>

#define FIFO   "/tmp/sps.fifo"
#define META   "/tmp/shairport-sync-ap2-metadata"
#define NLED   36
#define FRAME  2400
#define HIST   64
#define K_VOLDOWN 114
#define K_VOLUP   115
#define K_VOICE   582

static int delay_frames = 8;

static long now_ms(void){
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static void run(const char *c){ if (system(c)) {} }

static int vol_shown = -1;
static int last_flash_pct = -1;
static int overlay;
static long overlay_end;
static long cooldown_until;
static int fb2 = -1;
static unsigned char fb_save[NLED * 3];
static int fb_saved;

static int get_master(void){
  FILE *p = popen("amixer -c0 sget Master 2>/dev/null", "r");
  int v = -1;
  char l[200];
  if (p){
    while (fgets(l, sizeof l, p)){
      char *b = strchr(l, '[');
      if (b && strchr(b, '%')) v = atoi(b + 1);
    }
    pclose(p);
  }
  return v;
}

static void overlay_stop(void);

static void show_volume(int pct){
  long t = now_ms();
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  vol_shown = pct;
  if (overlay) return; /* update bar via vol_shown; do not extend the flash */
  if (t < cooldown_until) return;
  if (last_flash_pct >= 0 && abs(pct - last_flash_pct) < 2) return;
  last_flash_pct = pct;
  overlay = 1;
  overlay_end = t + 1600;
  if (fb2 < 0) fb2 = open("/dev/fb2", O_RDWR);
  fb_saved = 0;
  if (fb2 >= 0){
    if (lseek(fb2, 0, SEEK_SET) == 0 && read(fb2, fb_save, sizeof fb_save) == (ssize_t)sizeof fb_save)
      fb_saved = 1;
  }
}

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

/* ---- ring: orion RGB, 36 segments ---- */
static int ring_fd[NLED][3];
static int ring_ready;
static unsigned char last_px[NLED * 3];
static int px_valid;
static int grabbed;

static void ring_set_trigger(const char *trig, const char *sync_trig){
  char p[128];
  static const char *cn[3] = {"red", "green", "blue"};
  for (int i = 0; i < NLED; i++) for (int c = 0; c < 3; c++){
    int fd;
    snprintf(p, sizeof p, "/sys/class/leds/orion:%s:ring%d/trigger", cn[c], i);
    fd = open(p, O_WRONLY);
    if (fd >= 0){ if (write(fd, trig, strlen(trig)) < 0) {} close(fd); }
  }
  static const char *sn[3] = {"ringA", "ringB", "ringC"};
  for (int i = 0; i < 3; i++){
    int fd;
    snprintf(p, sizeof p, "/sys/class/leds/orion:%s:sync/trigger", sn[i]);
    fd = open(p, O_WRONLY);
    if (fd >= 0){ if (write(fd, sync_trig, strlen(sync_trig)) < 0) {} close(fd); }
  }
}

static void ring_grab(void){
  if (grabbed) return;
  ring_set_trigger("none", "none");
  grabbed = 1;
}

static void ring_release(void){
  if (!grabbed) return;
  ring_set_trigger("ledfb-ring", "ledfb-ring-sync");
  grabbed = 0;
  px_valid = 0;
  if (fb_saved && fb2 >= 0){
    if (lseek(fb2, 0, SEEK_SET) == 0){
      if (write(fb2, fb_save, sizeof fb_save) < 0) {}
    }
  }
}

static void overlay_stop(void){
  if (!overlay) return;
  overlay = 0;
  cooldown_until = now_ms() + 500;
  ring_release();
}

static void ring_open(void){
  static const char *cn[3] = {"red", "green", "blue"};
  char p[128];
  ring_grab();
  for (int i = 0; i < NLED; i++) for (int c = 0; c < 3; c++){
    snprintf(p, sizeof p, "/sys/class/leds/orion:%s:ring%d/brightness", cn[c], i);
    ring_fd[i][c] = open(p, O_WRONLY);
  }
  ring_ready = 1;
}

static void fb_write(const unsigned char *rgb){
  char b[8];
  int n;
  if (px_valid && !memcmp(last_px, rgb, sizeof last_px)) return;
  if (!ring_ready) ring_open();
  else ring_grab();
  for (int i = 0; i < NLED; i++) for (int c = 0; c < 3; c++){
    if (px_valid && last_px[i * 3 + c] == rgb[i * 3 + c]) continue;
    if (ring_fd[i][c] < 0) continue;
    n = snprintf(b, sizeof b, "%d\n", rgb[i * 3 + c]);
    if (pwrite(ring_fd[i][c], b, n, 0) < 0) {}
  }
  memcpy(last_px, rgb, sizeof last_px);
  px_valid = 1;
}

static void hsv(float h, float v, unsigned char *r, unsigned char *g, unsigned char *b){
  float c = v, hh = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
  float x = c * (1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f)), rr = 0, gg = 0, bb = 0;
  if (hh < 1){ rr = c; gg = x; } else if (hh < 2){ rr = x; gg = c; }
  else if (hh < 3){ gg = c; bb = x; } else if (hh < 4){ gg = x; bb = c; }
  else if (hh < 5){ rr = x; bb = c; } else { rr = c; bb = x; }
  *r = (unsigned char)(rr * 255);
  *g = (unsigned char)(gg * 255);
  *b = (unsigned char)(bb * 255);
}

static float hist[HIST];
static unsigned hpos;
static double sumsq;
static int nsmp;
static float smooth, prev_lv, avg_db = -25.0f;
static long last_audio_ms;
static float hue = 200.0f;

static void push_level(float lv){
  smooth = lv > smooth ? lv : smooth * 0.8f + lv * 0.2f;
  if (lv > prev_lv + 0.25f) hue += 28.0f;
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
      float lv = 0;
      if (db > -58.0f){
        avg_db += (db - avg_db) * 0.03f;
        lv = 0.5f + (db - avg_db) / 11.0f;
      }
      push_level(lv < 0 ? 0 : lv > 1 ? 1 : lv);
      sumsq = 0; nsmp = 0;
    }
  }
  last_audio_ms = now_ms();
}

static void render(int active){
  unsigned char px[NLED * 3];
  (void)active;
  if (overlay && now_ms() >= overlay_end) overlay_stop();
  if (!overlay){
    if (grabbed) ring_release();
    return;
  }

  memset(px, 0, sizeof px);
  for (int i = 0; i < NLED; i++){
    float f = vol_shown / 100.0f * NLED - i;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    hsv(120.0f - 120.0f * vol_shown / 100.0f, f * 0.8f, &px[i * 3], &px[i * 3 + 1], &px[i * 3 + 2]);
  }
  fb_write(px);
}

static int b64v(int c){
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}

static void handle_pvol(const char *b64){
  char out[96];
  int n = 0, acc = 0, bits = 0;
  for (const char *p = b64; *p && n < 90; p++){
    int v = b64v(*p);
    if (v < 0) continue;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8){ bits -= 8; out[n++] = (acc >> bits) & 0xff; }
  }
  out[n] = 0;
  float vol = strtof(out, NULL);
  int pct = vol <= -100.0f ? 0 : (int)(100.0f * powf(10.0f, vol / 40.0f) + 0.5f);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (pct == vol_shown) return;
  set_master(pct);
}

static char mbuf[8192];
static int mlen;

static void meta_feed(const char *d, int n){
  if (mlen + n >= (int)sizeof mbuf) mlen = 0;
  memcpy(mbuf + mlen, d, n);
  mlen += n;
  mbuf[mlen] = 0;
  for (;;){
    char *s = strstr(mbuf, "<item>"), *e = s ? strstr(s, "</item>") : NULL;
    if (!e){ if (!s) mlen = 0; return; }
    e += 7;
    if (strstr(s, "<type>73736e63</type>") && strstr(s, "<code>70766f6c</code>")){
      char *dp = strstr(s, "<data encoding=\"base64\">");
      if (dp && dp < e){
        dp += 24;
        char *q = strstr(dp, "</data>");
        if (q){ *q = 0; handle_pvol(dp); }
      }
    }
    memmove(mbuf, e, mlen - (e - mbuf) + 1);
    mlen -= e - mbuf;
  }
}

static int open_ro(const char *p){ return open(p, O_RDONLY | O_NONBLOCK); }

static void send_cmd(const char *name){
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return;
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(7001);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (sendto(s, name, strlen(name), 0, (struct sockaddr *)&a, sizeof a) < 0) {}
  close(s);
}

static int pend_key[8], pend_n;
static long pend_last;

static void flush_keys(void){
  int has[3] = {0, 0, 0}, first = 0;
  for (int i = 0; i < pend_n; i++){
    int k = pend_key[i];
    if (k == K_VOLDOWN) has[0] = 1;
    else if (k == K_VOLUP) has[1] = 1;
    else if (k == K_VOICE || k == KEY_PLAYPAUSE || k == KEY_PLAY || k == KEY_PAUSE) has[2] = 1;
    if (!first && (k == K_VOLDOWN || k == K_VOLUP)) first = k;
  }
  pend_n = 0;
  if (has[0] + has[1] + has[2] >= 2){
    int fwd = first == K_VOLDOWN;
    send_cmd(fwd ? "next" : "previous");
    return;
  }
  if (has[1]) master_step(1);
  else if (has[0]) master_step(-1);
  else if (has[2]) send_cmd("playpause");
}

static int open_keys(void){
  DIR *d = opendir("/dev/input");
  if (!d) return open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
  int found = -1;
  struct dirent *e;
  while ((e = readdir(d))){
    if (strncmp(e->d_name, "event", 5) != 0) continue;
    char p[64];
    snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
    int fd = open(p, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;
    unsigned long ev = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), &ev) == 0 && (ev & (1 << EV_KEY))){
      found = fd;
      break;
    }
    close(fd);
  }
  closedir(d);
  return found;
}

int main(int argc, char **argv){
  if (argc > 2 && !strcmp(argv[1], "-d")) delay_frames = atoi(argv[2]);
  mkfifo(FIFO, 0666);
  int ffd = open(FIFO, O_RDWR | O_NONBLOCK);
  int mfd = open_ro(META);
  int kfd = -1; /* Alice owns the touch ring; do not steal keys */
  long last_meta_try = 0;
  unsigned char abuf[16384];
  int aleft = 0;
  last_audio_ms = 0;
  render(0);

  for (;;){
    struct pollfd p[3];
    int np = 0, ia = -1, im = -1, ik = -1;
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
          char t[2048];
          ssize_t n = read(mfd, t, sizeof t);
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
    long now = now_ms();
    if (mfd < 0 && now - last_meta_try > 1000){ last_meta_try = now; mfd = open_ro(META); }
    int active = last_audio_ms && (now - last_audio_ms) < 2000;
    if (!active || r == 0) push_level(0);
    render(active);
  }
  ring_release();
  return 0;
}
