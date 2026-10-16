/* whycant -- explain why a user can or cannot access a path

   "Permission denied" says *that* access failed, never *where* or *why*.
   whycant walks PATH the way the Linux kernel does -- one component at
   a time, following symbolic links -- and at every step applies the
   same permission rules the kernel applies.  It stops at the first step
   that would fail, explains it in plain words, and suggests the
   narrowest fix (never chmod 777).

   Exit status: 0 access allowed, 1 access denied, 2 trouble
   (bad usage, unknown user, a path that cannot be inspected).

   Modelled:   owner/group/other classes (first match wins, no fallback),
               POSIX ACLs (named users and groups, and the mask), root's
               override of the mode bits, read-only and noexec mounts,
               symlinks (40 at most, like the kernel), "." and "..",
               trailing slashes, and creating a new entry (-c).
   Not modelled: capabilities other than full root, immutable
               attributes, SELinux/AppArmor, and network-filesystem rules
               such as NFS root_squash.

   Run it as root to inspect paths that the caller itself cannot see.

   Build:  cc -std=c17 -O2 -Wall -Wextra -o whycant whycant.c        */

#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <stdarg.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#define PROGRAM_NAME "whycant"
#define PROGRAM_VERSION "1.1"

/* The kernel gives up with ELOOP after this many symlinks in one lookup.  */
enum { MAX_SYMLINKS = 40 };

enum { EXIT_ALLOWED = 0, EXIT_DENIED = 1, EXIT_TROUBLE = 2 };

/* Permission bits, valued as chmod writes them: r=4 w=2 x=1.  */
enum { P_R = 4, P_W = 2, P_X = 1 };

/* Which set of mode bits the kernel consults for this user.  */
enum perm_class { CLASS_ROOT, CLASS_OWNER, CLASS_ACL_USER, CLASS_GROUP,
                  CLASS_OTHER };

static char const *const class_name[] = {
  [CLASS_ROOT] = "root",
  [CLASS_OWNER] = "owner",
  [CLASS_ACL_USER] = "named user",
  [CLASS_GROUP] = "group",
  [CLASS_OTHER] = "other",
};

/* The user whose access is being explained.  */
struct user
{
  uid_t uid;
  gid_t gid;                    /* Primary group.  */
  gid_t *groups;                /* Supplementary groups.  */
  int ngroups;
  bool has_passwd_entry;
  char name[256];
};

/* The outcome of one permission check.  */
struct verdict
{
  bool ok;
  enum perm_class cls;
  unsigned have;                /* Bits the user's class grants.  */
  unsigned missing;             /* Requested bits that are not granted.  */
  char const *blocker;          /* A reason no chmod can fix, or NULL.  */
  bool acl;                     /* The decision came from an ACL.  */
  bool masked;                  /* An ACL entry allowed it, the mask did not.  */
  unsigned entry_perm;          /* That ACL entry's own bits, before the mask.  */
  unsigned acl_mask;            /* The ACL mask (7 if there is none).  */
  char via[160];                /* "user:bob", "group:devs", ... for ACLs.  */
};

/* --json collects one result per PATH and prints it as one line.  */
static bool g_json;

/* ANSI colours; left empty unless stdout is a terminal.  */
static char const *c_ok = "", *c_bad = "", *c_dim = "", *c_bold = "",
  *c_off = "";

/* The original command line, to print an exact "sudo ..." suggestion.  */
static int g_argc;
static char **g_argv;

static void
setup_colours (void)
{
  char const *no_color = getenv ("NO_COLOR");
  if (isatty (STDOUT_FILENO) && !(no_color && *no_color))
    {
      c_ok = "\033[32m";
      c_bad = "\033[31m";
      c_dim = "\033[2m";
      c_bold = "\033[1m";
      c_off = "\033[0m";
    }
}

/* ---------------------------------------------------------------------
   Safe output.  File names may contain any byte except '/' and NUL,
   including newlines and terminal escape sequences.
   --------------------------------------------------------------------- */

static bool
is_control (unsigned char c)
{
  return c < 0x20 || c == 0x7f;
}

/* Print S so that control characters cannot disturb the terminal.  */
static void
put_escaped (FILE *out, char const *s)
{
  for (unsigned char const *p = (unsigned char const *) s; *p; p++)
    {
      if (is_control (*p))
        fprintf (out, "\\%03o", *p);
      else if (*p == '\\')
        fputs ("\\\\", out);
      else
        putc (*p, out);
    }
}

/* Print S as a single shell word that is safe to copy and paste.  */
static void
fput_shell_word (FILE *out, char const *s)
{
  bool plain = *s != '\0';
  bool has_control = false;
  for (unsigned char const *p = (unsigned char const *) s; *p; p++)
    {
      if (is_control (*p))
        has_control = true;
      if (!(('a' <= *p && *p <= 'z') || ('A' <= *p && *p <= 'Z')
            || ('0' <= *p && *p <= '9') || strchr ("_@%+=:,./-", *p)))
        plain = false;
    }

  if (plain)
    fputs (s, out);
  else if (!has_control)
    {
      putc ('\'', out);
      for (char const *p = s; *p; p++)
        if (*p == '\'')
          fputs ("'\\''", out);
        else
          putc (*p, out);
      putc ('\'', out);
    }
  else
    {
      /* Bash/zsh/ksh ANSI-C quoting, the only portable-enough way to
         spell a newline inside a single word.  */
      fputs ("$'", out);
      for (unsigned char const *p = (unsigned char const *) s; *p; p++)
        {
          if (*p == '\'' || *p == '\\')
            fprintf (out, "\\%c", *p);
          else if (is_control (*p))
            fprintf (out, "\\%03o", *p);
          else
            putc (*p, out);
        }
      putc ('\'', out);
    }
}

static void
put_shell_word (char const *s)
{
  fput_shell_word (stdout, s);
}

static char *g_last_error;      /* For --json: the last error message.  */

static void
warn_path (char const *what, char const *path, int err, char const *hint)
{
  fprintf (stderr, "%s: %s '", PROGRAM_NAME, what);
  put_escaped (stderr, path);
  fprintf (stderr, "': %s%s\n", strerror (err), hint ? hint : "");
  free (g_last_error);
  g_last_error = NULL;
  if (asprintf (&g_last_error, "%s '%s': %s", what, path, strerror (err)) < 0)
    g_last_error = NULL;
}

/* ---------------------------------------------------------------------
   Small formatting helpers.
   --------------------------------------------------------------------- */

/* "rx" style: only the bits that are set.  */
static void
bits_to_str (unsigned bits, char out[4])
{
  char *p = out;
  if (bits & P_R)
    *p++ = 'r';
  if (bits & P_W)
    *p++ = 'w';
  if (bits & P_X)
    *p++ = 'x';
  *p = '\0';
}

/* "r-x" style: all three positions.  */
static void
bits_to_triplet (unsigned bits, char out[4])
{
  out[0] = bits & P_R ? 'r' : '-';
  out[1] = bits & P_W ? 'w' : '-';
  out[2] = bits & P_X ? 'x' : '-';
  out[3] = '\0';
}

/* "drwxr-x---+" style, as ls -l prints it.  */
static void
format_mode (mode_t m, bool acl, char out[12])
{
  out[0] = (S_ISDIR (m) ? 'd' : S_ISLNK (m) ? 'l' : S_ISCHR (m) ? 'c'
            : S_ISBLK (m) ? 'b' : S_ISFIFO (m) ? 'p' : S_ISSOCK (m) ? 's'
            : '-');
  out[1] = m & S_IRUSR ? 'r' : '-';
  out[2] = m & S_IWUSR ? 'w' : '-';
  out[3] = (m & S_ISUID ? (m & S_IXUSR ? 's' : 'S')
            : (m & S_IXUSR ? 'x' : '-'));
  out[4] = m & S_IRGRP ? 'r' : '-';
  out[5] = m & S_IWGRP ? 'w' : '-';
  out[6] = (m & S_ISGID ? (m & S_IXGRP ? 's' : 'S')
            : (m & S_IXGRP ? 'x' : '-'));
  out[7] = m & S_IROTH ? 'r' : '-';
  out[8] = m & S_IWOTH ? 'w' : '-';
  out[9] = (m & S_ISVTX ? (m & S_IXOTH ? 't' : 'T')
            : (m & S_IXOTH ? 'x' : '-'));
  out[10] = acl ? '+' : '\0';
  out[11] = '\0';
}

static void
uid_to_name (uid_t uid, char *buf, size_t size)
{
  struct passwd const *pw = getpwuid (uid);
  if (pw)
    snprintf (buf, size, "%s", pw->pw_name);
  else
    snprintf (buf, size, "%lu", (unsigned long) uid);
}

static void
gid_to_name (gid_t gid, char *buf, size_t size)
{
  struct group const *gr = getgrgid (gid);
  if (gr)
    snprintf (buf, size, "%s", gr->gr_name);
  else
    snprintf (buf, size, "%lu", (unsigned long) gid);
}

/* Join DIR and NAME into OUT.  Return false if it would not fit.  */
static bool
join_path (char *out, size_t size, char const *dir, char const *name)
{
  size_t dlen = strlen (dir);
  size_t nlen = strlen (name);
  bool root = dlen == 1 && dir[0] == '/';
  size_t total = dlen + !root + nlen;
  if (total >= size)
    return false;
  memcpy (out, dir, dlen);
  if (!root)
    out[dlen] = '/';
  memcpy (out + dlen + !root, name, nlen + 1);
  return true;
}

/* Replace the absolute path PATH with its parent; "/" stays "/".  */
static void
to_parent (char *path)
{
  char *slash = strrchr (path, '/');
  if (slash == path)
    path[1] = '\0';
  else if (slash)
    *slash = '\0';
}

/* ---------------------------------------------------------------------
   The user being explained.
   --------------------------------------------------------------------- */

static bool
load_groups (struct user *u)
{
  int n = 32;
  for (;;)
    {
      gid_t *g = realloc (u->groups, n * sizeof *g);
      if (!g)
        return false;
      u->groups = g;
      int want = n;
      if (getgrouplist (u->name, u->gid, u->groups, &want) != -1)
        {
          u->ngroups = want;
          return true;
        }
      /* glibc reports the size it needs; be defensive if it did not.  */
      n = want > n ? want : n * 2;
    }
}

/* Fill U for SPEC (a user name or numeric uid), or for the caller
   when SPEC is NULL.  Return false after printing a diagnostic.  */
static bool
load_user (char const *spec, struct user *u)
{
  *u = (struct user) { 0 };

  /* Under sudo, explain access for the person who typed sudo, not for
     root (who may open almost anything).  "-u root" still checks root.  */
  if (!spec && geteuid () == 0)
    {
      char const *sudo_user = getenv ("SUDO_USER");
      if (sudo_user && *sudo_user)
        spec = sudo_user;
    }

  if (!spec)
    {
      u->uid = geteuid ();
      u->gid = getegid ();
      int n = getgroups (0, NULL);
      if (n < 0 || !(u->groups = malloc ((n + 1) * sizeof *u->groups)))
        goto oom;
      u->ngroups = getgroups (n, u->groups);
      if (u->ngroups < 0)
        u->ngroups = 0;
      struct passwd const *pw = getpwuid (u->uid);
      u->has_passwd_entry = pw != NULL;
      uid_to_name (u->uid, u->name, sizeof u->name);
      return true;
    }

  struct passwd const *pw = getpwnam (spec);
  if (!pw)
    {
      char *end;
      errno = 0;
      unsigned long n = strtoul (spec, &end, 10);
      if (!*spec || *end || errno || spec[0] == '-'
          || n != (unsigned long) (uid_t) n)
        {
          fprintf (stderr, "%s: no such user: ", PROGRAM_NAME);
          put_escaped (stderr, spec);
          putc ('\n', stderr);
          return false;
        }
      pw = getpwuid ((uid_t) n);
      if (!pw)
        {
          /* A bare uid with no account: it owns files but has no groups.  */
          u->uid = (uid_t) n;
          u->gid = (gid_t) -1;
          snprintf (u->name, sizeof u->name, "%lu", n);
          fprintf (stderr, "%s: warning: uid %lu has no passwd entry; "
                   "assuming it belongs to no groups\n", PROGRAM_NAME, n);
          return true;
        }
    }

  u->uid = pw->pw_uid;
  u->gid = pw->pw_gid;
  u->has_passwd_entry = true;
  snprintf (u->name, sizeof u->name, "%s", pw->pw_name);
  if (!load_groups (u))
    goto oom;
  return true;

 oom:
  fprintf (stderr, "%s: cannot load groups: %s\n", PROGRAM_NAME,
           strerror (errno ? errno : ENOMEM));
  return false;
}

static bool
user_in_group (struct user const *u, gid_t gid)
{
  if (u->gid == gid)
    return true;
  for (int i = 0; i < u->ngroups; i++)
    if (u->groups[i] == gid)
      return true;
  return false;
}

/* ---------------------------------------------------------------------
   The permission check itself, mirroring the kernel's generic_permission.
   --------------------------------------------------------------------- */

/* POSIX ACLs, read straight from the "system.posix_acl_access" extended
   attribute, so no libacl is needed.  Layout (little-endian): a 4-byte
   version (2), then 8-byte entries { u16 tag; u16 perm; u32 id }.  */
enum
{
  ACL_TAG_USER_OBJ = 0x01, ACL_TAG_USER = 0x02, ACL_TAG_GROUP_OBJ = 0x04,
  ACL_TAG_GROUP = 0x08, ACL_TAG_MASK = 0x10, ACL_TAG_OTHER = 0x20,
  ACL_MAX_ENTRIES = 64
};

struct acl_entry
{
  unsigned tag, perm;
  unsigned long id;
};

/* Return the number of entries (0 if there is no ACL), or -1 if the ACL
   exists but cannot be read or understood.  */
static int
read_acl (char const *path, struct acl_entry *e, int max)
{
  unsigned char buf[4 + 8 * ACL_MAX_ENTRIES];
  ssize_t n = getxattr (path, "system.posix_acl_access", buf, sizeof buf);
  if (n < 0)
    return errno == ENODATA || errno == ENOTSUP ? 0 : -1;
  if (n < 4 || (n - 4) % 8 != 0
      || (buf[0] | buf[1] << 8 | buf[2] << 16 | (unsigned) buf[3] << 24) != 2)
    return -1;
  int count = (int) ((n - 4) / 8);
  if (count > max)
    return -1;
  for (int i = 0; i < count; i++)
    {
      unsigned char const *q = buf + 4 + 8 * i;
      e[i].tag = (unsigned) (q[0] | q[1] << 8);
      e[i].perm = (unsigned) (q[2] | q[3] << 8) & 7;
      e[i].id = (unsigned long) q[4] | (unsigned long) q[5] << 8
                | (unsigned long) q[6] << 16 | (unsigned long) q[7] << 24;
    }
  return count;
}

static bool
has_access_acl (char const *path)
{
  return getxattr (path, "system.posix_acl_access", NULL, 0) > 0;
}

static unsigned long
mount_flags (char const *path)
{
  struct statvfs sv;
  return statvfs (path, &sv) == 0 ? sv.f_flag : 0;
}

/* The kernel's posix_acl_permission(), for a user who is not the owner.
   Returns true and fills V when the ACL decides; false means "no usable
   ACL, use the mode bits".  */
static bool
evaluate_acl (char const *path, struct stat const *st, unsigned want,
              struct user const *u, struct verdict *v)
{
  struct acl_entry e[ACL_MAX_ENTRIES];
  int n = read_acl (path, e, ACL_MAX_ENTRIES);
  if (n <= 0)
    return false;

  unsigned mask = 7;
  bool has_mask = false;
  for (int i = 0; i < n; i++)
    if (e[i].tag == ACL_TAG_MASK)
      {
        mask = e[i].perm;
        has_mask = true;
      }

  v->acl = true;
  v->acl_mask = mask;
  bool found = false;
  unsigned group_union = 0;
  char name[128];
  for (int i = 0; i < n; i++)
    {
      switch (e[i].tag)
        {
        case ACL_TAG_USER:
          if ((uid_t) e[i].id != u->uid)
            break;
          uid_to_name ((uid_t) e[i].id, name, sizeof name);
          snprintf (v->via, sizeof v->via, "user:%s", name);
          v->cls = CLASS_ACL_USER;
          v->entry_perm = e[i].perm;
          v->have = e[i].perm & mask;
          v->masked = has_mask && (e[i].perm & want) == want
                      && (v->have & want) != want;
          return true;

        case ACL_TAG_GROUP_OBJ:
        case ACL_TAG_GROUP:
          {
            gid_t gid = e[i].tag == ACL_TAG_GROUP_OBJ
                        ? st->st_gid : (gid_t) e[i].id;
            if (!user_in_group (u, gid))
              break;
            found = true;
            group_union |= e[i].perm;
            if ((e[i].perm & want) != want)
              break;
            /* The first group entry that allows it decides, after the mask.  */
            gid_to_name (gid, name, sizeof name);
            snprintf (v->via, sizeof v->via, "group:%s", name);
            v->cls = CLASS_GROUP;
            v->entry_perm = e[i].perm;
            v->have = e[i].perm & mask;
            v->masked = has_mask && (v->have & want) != want;
            return true;
          }

        case ACL_TAG_OTHER:
          if (found)
            {
              /* In a matching group, but none of those entries allows it:
                 the kernel denies without looking at "other".  */
              snprintf (v->via, sizeof v->via, "the user's groups");
              v->cls = CLASS_GROUP;
              v->entry_perm = group_union;
              v->have = group_union & mask;
              v->masked = false;
              return true;
            }
          snprintf (v->via, sizeof v->via, "other");
          v->cls = CLASS_OTHER;
          v->entry_perm = e[i].perm;
          v->have = e[i].perm;
          return true;

        default:
          break;
        }
    }
  v->acl = false;
  return false;
}

static struct verdict
evaluate (char const *path, struct stat const *st, unsigned want,
          struct user const *u)
{
  struct verdict v = { 0 };
  mode_t m = st->st_mode;

  if (u->uid == 0)
    {
      /* Root may read and write anything, and search any directory,
         but may execute a file only if some x bit is set.  */
      v.cls = CLASS_ROOT;
      v.have = P_R | P_W;
      if (S_ISDIR (m) || (m & (S_IXUSR | S_IXGRP | S_IXOTH)))
        v.have |= P_X;
    }
  else if (st->st_uid == u->uid)
    {
      v.cls = CLASS_OWNER;
      v.have = (m >> 6) & 7;
    }
  /* Like the kernel, use the ACL only when the group bits (its mask) are
     not all zero.  */
  else if ((m & S_IRWXG) && evaluate_acl (path, st, want, u, &v))
    ;
  else if (user_in_group (u, st->st_gid))
    {
      v.cls = CLASS_GROUP;
      v.have = (m >> 3) & 7;
    }
  else
    {
      v.cls = CLASS_OTHER;
      v.have = m & 7;
    }
  v.missing = want & ~v.have;

  /* Mount options apply to everyone, root included.  */
  unsigned long mflags = mount_flags (path);
  if ((want & P_W) && (mflags & ST_RDONLY))
    v.blocker = "filesystem is mounted read-only";
  else if ((want & P_X) && S_ISREG (m) && (mflags & ST_NOEXEC))
    v.blocker = "filesystem is mounted noexec";

  v.ok = !v.missing && !v.blocker;
  return v;
}

/* ---------------------------------------------------------------------
   Reporting.
   --------------------------------------------------------------------- */

/* ---------------------------------------------------------------------
   JSON output (--json): one object per PATH, on one line.
   --------------------------------------------------------------------- */

/* Write S as a JSON string.  File names are bytes, not text: invalid
   UTF-8 is replaced with U+FFFD so the output always parses.  */
static void
json_str (FILE *out, char const *s)
{
  putc ('"', out);
  for (unsigned char const *p = (unsigned char const *) s; *p; )
    {
      unsigned char c = *p;
      if (c == '"' || c == '\\')
        fprintf (out, "\\%c", c), p++;
      else if (c < 0x20 || c == 0x7f)
        fprintf (out, "\\u%04x", c), p++;
      else if (c < 0x80)
        putc (c, out), p++;
      else
        {
          int len = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
          if (c >= 0xf5)
            len = 0;
          bool valid = len > 0;
          for (int k = 1; valid && k < len; k++)
            valid = (p[k] & 0xc0) == 0x80;
          if (valid && len == 3 && ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] >= 0xa0)))
            valid = false;
          if (valid && len == 4 && ((c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] >= 0x90)))
            valid = false;
          if (valid)
            {
              fwrite (p, 1, (size_t) len, out);
              p += len;
            }
          else
            {
              fputs ("\\ufffd", out);
              p++;
            }
        }
    }
  putc ('"', out);
}

/* Everything known about one PATH, printed at the end in JSON mode.  */
static struct
{
  FILE *steps;                  /* Comma-separated step objects.  */
  char *steps_buf;
  size_t steps_len;
  int nsteps;
  char problem[160];            /* e.g. "does not exist".  */
  char *problem_path;
  char *sudo_command;
  char *error;
  bool has_advice;
  bool allowed;
} J;

static void
json_reset (void)
{
  if (J.steps)
    fclose (J.steps);
  free (J.steps_buf);
  free (J.problem_path);
  free (J.sudo_command);
  free (J.error);
  memset (&J, 0, sizeof J);
  J.steps = open_memstream (&J.steps_buf, &J.steps_len);
}

static FILE *
json_step_open (void)
{
  if (J.nsteps++)
    putc (',', J.steps);
  return J.steps;
}

/* ---------------------------------------------------------------------
   Advice: why a step failed and the narrowest fixes.  Built once, then
   printed for people or as JSON.
   --------------------------------------------------------------------- */

enum { MAX_FIXES = 4 };

struct advice
{
  char why[1024];
  char note[512];               /* Extra hints: x on folders, ACL details.  */
  char stop[256];               /* Set when no fix must be suggested.  */
  char const *blocker_fix;      /* For read-only / noexec mounts.  */
  int nfix;
  struct
  {
    char *cmd;
    char scope[128];
  } fix[MAX_FIXES];
};

static struct advice g_advice;  /* The advice for the current PATH.  */

static void
advice_free (struct advice *a)
{
  for (int i = 0; i < a->nfix; i++)
    free (a->fix[i].cmd);
  memset (a, 0, sizeof *a);
}

/* Add a fix "WORD1 WORD2 ..." where each word is shell-quoted.  */
static void
add_fix (struct advice *a, char const *scope, int nwords, ...)
{
  if (a->nfix >= MAX_FIXES)
    return;
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream (&buf, &len);
  if (!f)
    return;
  va_list ap;
  va_start (ap, nwords);
  for (int i = 0; i < nwords; i++)
    {
      char const *w = va_arg (ap, char const *);
      if (i)
        putc (' ', f);
      /* Words starting with '\1' are literal shell syntax such as "&&".  */
      if (w[0] == '\1')
        fputs (w + 1, f);
      else
        fput_shell_word (f, w);
    }
  va_end (ap);
  if (fclose (f) != 0)
    {
      free (buf);
      return;
    }
  a->fix[a->nfix].cmd = buf;
  snprintf (a->fix[a->nfix].scope, sizeof a->fix[a->nfix].scope, "%s", scope);
  a->nfix++;
}

static void
note_add (struct advice *a, char const *text)
{
  size_t used = strlen (a->note);
  snprintf (a->note + used, sizeof a->note - used, "%s%s", used ? " " : "", text);
}

/* Files kept closed on purpose: passwords, keys and admin rules.
   Opening them would be a security hole, so no fix is suggested.  */
static bool
is_sensitive (char const *path)
{
  static char const *const exact[] = {
    "/etc/shadow", "/etc/shadow-", "/etc/gshadow", "/etc/gshadow-",
    "/etc/sudoers",
  };
  static char const *const trees[] = {
    "/etc/sudoers.d", "/etc/ssl/private", "/root",
  };

  for (size_t i = 0; i < sizeof exact / sizeof *exact; i++)
    if (strcmp (path, exact[i]) == 0)
      return true;

  /* "/root" must match "/root" and "/root/x", but not "/rootfs".  */
  for (size_t i = 0; i < sizeof trees / sizeof *trees; i++)
    {
      size_t n = strlen (trees[i]);
      if (strncmp (path, trees[i], n) == 0
          && (path[n] == '\0' || path[n] == '/'))
        return true;
    }

  /* SSH keys: anything inside a .ssh folder, and the server's own keys.  */
  size_t len = strlen (path);
  if (strstr (path, "/.ssh/")
      || (len >= 5 && strcmp (path + len - 5, "/.ssh") == 0)
      || (strncmp (path, "/etc/ssh/ssh_host_", 18) == 0
          && len >= 4 && strcmp (path + len - 4, "_key") == 0))
    return true;

  return false;
}

/* Groups whose members are effectively root: docker and lxd can start
   a container that mounts "/", disk and kmem read raw disks and memory,
   sudo/wheel/admin may run anything as root, and shadow reads every
   password hash.  Joining one, or getting access to its files, is far
   more than "one step", so whycant must never suggest it.  */
static bool
is_privileged_group (char const *name)
{
  static char const *const groups[] = {
    "sudo", "wheel", "admin", "docker", "lxd", "libvirt",
    "disk", "kmem", "shadow",
  };
  for (size_t i = 0; i < sizeof groups / sizeof *groups; i++)
    if (strcmp (name, groups[i]) == 0)
      return true;
  return false;
}

/* Work out why step V on PATH failed and which fixes to suggest.  */
static void
build_advice (char const *path, struct stat const *st,
              struct verdict const *v, struct user const *u, struct advice *a)
{
  char bits[4], group[128], owner[128], spec[300];
  advice_free (a);
  bits_to_str (v->missing, bits);
  gid_to_name (st->st_gid, group, sizeof group);
  uid_to_name (st->st_uid, owner, sizeof owner);

  if (v->blocker)
    {
      snprintf (a->why, sizeof a->why, "the %s; no chmod can change that.",
                v->blocker);
      a->blocker_fix = strstr (v->blocker, "read-only")
        ? "write somewhere else, or remount read-write if that is really intended."
        : "run it through its interpreter (e.g. sh FILE), or remount without noexec if that is really intended.";
      return;
    }

  char entry[4];
  bits_to_triplet (v->entry_perm, entry);
  switch (v->cls)
    {
    case CLASS_ROOT:
      snprintf (a->why, sizeof a->why,
                "even root may only execute a file that has an x bit.");
      break;
    case CLASS_OWNER:
      snprintf (a->why, sizeof a->why,
                "%s owns this, and the owner bits lack %s. "
                "The owner never falls back to the group or other bits.",
                u->name, bits);
      break;
    case CLASS_ACL_USER:
      snprintf (a->why, sizeof a->why,
                "an ACL entry for %s (%s) decides here, and %s. "
                "A named user never falls back to the group or other bits.",
                u->name, v->via,
                v->masked ? "the ACL mask removes the bits it would allow"
                          : "it lacks the requested bits");
      break;
    case CLASS_GROUP:
      if (v->acl && v->masked)
        snprintf (a->why, sizeof a->why,
                  "the ACL entry %s would allow it, but the ACL mask removes those bits.",
                  v->via);
      else if (v->acl)
        snprintf (a->why, sizeof a->why,
                  "%s matches group entries in this file's ACL, and none of them allows %s. "
                  "A group member never falls back to the other bits.",
                  u->name, bits);
      else
        snprintf (a->why, sizeof a->why,
                  "%s is in group %s, and the group bits lack %s. "
                  "A group member never falls back to the other bits.",
                  u->name, group, bits);
      break;
    case CLASS_OTHER:
      snprintf (a->why, sizeof a->why,
                "%s is not the owner and not in group %s, so the 'other' bits "
                "apply, and they lack %s.", u->name, group, bits);
      break;
    }
  if (S_ISDIR (st->st_mode) && (v->missing & P_X))
    note_add (a, "(x on a directory means \"may pass through\"; listing it needs r.)");
  if (v->acl)
    note_add (a, "This file has an ACL ('+'); whycant read it like the kernel does.");

  /* Some locks must stay locked: say why, and suggest nothing.  */
  if (is_sensitive (path))
    snprintf (a->stop, sizeof a->stop,
              "closed on purpose (passwords, keys or admin rules)");
  else if (S_ISBLK (st->st_mode))
    snprintf (a->stop, sizeof a->stop,
              "a raw disk; reading it bypasses every file permission");
  else if (st->st_gid != 0 && is_privileged_group (group))
    snprintf (a->stop, sizeof a->stop,
              "guarded by group %s, and access to it is as good as root",
              group);
  if (*a->stop)
    return;

  /* A group named after its owner is that user's personal group:
     never suggest joining it.  */
  bool personal_group = strcmp (group, owner) == 0;

  switch (v->cls)
    {
    case CLASS_ROOT:
      add_fix (a, "", 3, "chmod", "u+x", path);
      break;

    case CLASS_OWNER:
      snprintf (spec, sizeof spec, "u+%s", bits);
      add_fix (a, "", 3, "chmod", spec, path);
      break;

    case CLASS_ACL_USER:
      if (v->masked)
        {
          char newmask[4];
          bits_to_str (v->acl_mask | v->missing, newmask);
          snprintf (spec, sizeof spec, "m::%s", newmask);
          add_fix (a, "raise the ACL mask; it also limits the group and every named entry", 4,
                   "setfacl", "-m", spec, path);
        }
      else
        {
          char all[4];
          bits_to_str (v->entry_perm | v->missing, all);
          snprintf (spec, sizeof spec, "u:%s:%s", u->name, all);
          add_fix (a, "only this user; replaces their ACL entry", 4,
                   "setfacl", "-m", spec, path);
        }
      break;

    case CLASS_GROUP:
    case CLASS_OTHER:
      if (v->masked)
        {
          char newmask[4];
          bits_to_str (v->acl_mask | v->missing, newmask);
          snprintf (spec, sizeof spec, "m::%s", newmask);
          add_fix (a, "raise the ACL mask; it also limits the group and every named entry", 4,
                   "setfacl", "-m", spec, path);
          break;
        }
      snprintf (spec, sizeof spec, "u:%s:%s", u->name, bits);
      add_fix (a, "only this user; needs the acl package", 4,
               "setfacl", "-m", spec, path);

      if (v->cls == CLASS_GROUP)
        {
          if (!v->acl)
            {
              char gspec[16];
              snprintf (gspec, sizeof gspec, "g+%s", bits);
              char scope[160];
              snprintf (scope, sizeof scope, "every member of group %s", group);
              add_fix (a, scope, 3, "chmod", gspec, path);
            }
          break;
        }

      /* Joining the file's group is reasonable -- unless that group is
         root's, someone's personal group, or the file has an ACL (where
         chmod g+ would change the mask instead).  */
      if (u->has_passwd_entry && st->st_gid != 0 && !personal_group && !v->acl)
        {
          unsigned group_bits = (st->st_mode >> 3) & 7;
          if ((group_bits & v->missing) != v->missing)
            {
              char gspec[16];
              snprintf (gspec, sizeof gspec, "g+%s", bits);
              add_fix (a, "every member of the group; takes effect at next login",
                       8, "usermod", "-aG", group, u->name, "\1&&", "chmod", gspec, path);
            }
          else
            add_fix (a, "every member of the group; takes effect at next login",
                     4, "usermod", "-aG", group, u->name);
        }

      snprintf (spec, sizeof spec, "o+%s", bits);
      add_fix (a, "every user on the system -- use only for public content",
               3, "chmod", spec, path);
      break;
    }
}

/* Print the advice for people.  */
static void
print_advice (struct advice const *a)
{
  printf ("\n  %sWhy:%s %s\n", c_bold, c_off, a->why);
  if (*a->note)
    printf ("       %s\n", a->note);
  if (a->blocker_fix)
    {
      printf ("  %sFix:%s %s\n", c_bold, c_off, a->blocker_fix);
      return;
    }
  if (*a->stop)
    {
      printf ("\n  %sStop:%s this is %s.\n"
              "       Opening it would be a security hole, so no fix is "
              "suggested.\n"
              "       If you really need it once, use sudo for that one "
              "command.\n", c_bad, c_off, a->stop);
      return;
    }
  printf ("\n  %sNarrowest fix first:%s\n", c_bold, c_off);
  for (int i = 0; i < a->nfix; i++)
    {
      printf ("    %d) %s", i + 1, a->fix[i].cmd);
      if (*a->fix[i].scope)
        printf ("\n       %s# %s%s", c_dim, a->fix[i].scope, c_off);
      putchar ('\n');
    }
  printf ("  %sAvoid chmod 777 and chmod -R: they open far more than this "
          "one step.%s\n", c_dim, c_off);
}

static void
suggest_fixes (char const *path, struct stat const *st,
               struct verdict const *v, struct user const *u)
{
  build_advice (path, st, v, u, &g_advice);
  if (g_json)
    J.has_advice = true;
  else
    print_advice (&g_advice);
}

/* One line of the walk: verdict, mode, owner:group, path, reason.  */
static void
report_step (char const *path, struct stat const *st, unsigned want,
             struct verdict const *v, char const *action)
{
  char mode[12], owner[128], group[128], og[260], need[4], got[4];
  format_mode (st->st_mode, has_access_acl (path), mode);
  uid_to_name (st->st_uid, owner, sizeof owner);
  gid_to_name (st->st_gid, group, sizeof group);
  bits_to_str (want, need);
  bits_to_triplet (v->have, got);

  if (g_json)
    {
      FILE *f = json_step_open ();
      fputs ("{\"path\":", f);
      json_str (f, path);
      fputs (",\"mode\":", f);
      json_str (f, mode);
      fputs (",\"owner\":", f);
      json_str (f, owner);
      fputs (",\"group\":", f);
      json_str (f, group);
      fprintf (f, ",\"action\":\"%s\",\"needs\":\"%s\",\"class\":\"%s\",\"gets\":\"%s\",\"ok\":%s",
               action, need, class_name[v->cls], got, v->ok ? "true" : "false");
      if (v->acl)
        {
          fputs (",\"acl_entry\":", f);
          json_str (f, v->via);
          fprintf (f, ",\"acl_masked\":%s", v->masked ? "true" : "false");
        }
      if (v->blocker)
        {
          fputs (",\"blocked_by\":", f);
          json_str (f, v->blocker);
        }
      putc ('}', f);
      return;
    }

  snprintf (og, sizeof og, "%s:%s", owner, group);
  printf ("  %s%s%s  %-11s %-16s ", v->ok ? c_ok : c_bad,
          v->ok ? "[ ok ]" : "[DENY]", c_off, mode, og);
  put_escaped (stdout, path);
  printf ("  %s%s needs %s: ", c_dim, action, need);
  if (v->blocker)
    fputs (v->blocker, stdout);
  else if (v->cls == CLASS_ROOT)
    fputs (v->ok ? "root ignores the mode bits"
           : "root still needs one x bit", stdout);
  else if (v->acl)
    {
      printf ("ACL %s gives %s", v->via, got);
      if (v->masked)
        fputs (" after the mask", stdout);
    }
  else
    printf ("as %s you get %s", class_name[v->cls], got);
  printf ("%s\n", c_off);
}

static void
report_link (char const *path, char const *target)
{
  if (g_json)
    {
      FILE *f = json_step_open ();
      fputs ("{\"link\":", f);
      json_str (f, path);
      fputs (",\"target\":", f);
      json_str (f, target);
      putc ('}', f);
      return;
    }
  printf ("  %s[link]%s  %-11s %-16s ", c_dim, c_off, "", "");
  put_escaped (stdout, path);
  fputs (" -> ", stdout);
  put_escaped (stdout, target);
  putchar ('\n');
}

static void
report_problem (char const *tag, char const *path, char const *what)
{
  if (g_json)
    {
      snprintf (J.problem, sizeof J.problem, "%s", what);
      free (J.problem_path);
      J.problem_path = strdup (path);
      (void) tag;
      return;
    }
  printf ("  %s%s%s  ", c_bad, tag, c_off);
  put_escaped (stdout, path);
  printf ("  %s%s%s\n", c_dim, what, c_off);
}

/* "read and write", "execute", ...  */
static void
describe_want (unsigned want, bool create, char *buf, size_t size)
{
  if (create)
    {
      snprintf (buf, size, "create");
      return;
    }
  char const *words[3];
  int n = 0;
  if (want & P_R)
    words[n++] = "read";
  if (want & P_W)
    words[n++] = "write";
  if (want & P_X)
    words[n++] = "execute";
  if (n == 1)
    snprintf (buf, size, "%s", words[0]);
  else if (n == 2)
    snprintf (buf, size, "%s and %s", words[0], words[1]);
  else
    snprintf (buf, size, "%s, %s and %s", words[0], words[1], words[2]);
}

/* The caller cannot see past PATH, but root can.  Print the exact
   command that lets the walk continue, instead of a bare error.  */
static void
suggest_sudo (char const *path)
{
  if (g_json)
    {
      char *buf = NULL;
      size_t len = 0;
      FILE *f = open_memstream (&buf, &len);
      if (f)
        {
          fputs ("sudo", f);
          for (int i = 0; i < g_argc; i++)
            {
              putc (' ', f);
              fput_shell_word (f, g_argv[i]);
            }
          if (fclose (f) == 0)
            J.sudo_command = buf;
          else
            free (buf);
        }
      snprintf (J.problem, sizeof J.problem, "cannot look inside this without root");
      free (J.problem_path);
      J.problem_path = strdup (path);
      return;
    }
  printf ("  %s[ROOT]%s  ", c_bad, c_off);
  put_escaped (stdout, path);
  printf ("  %syou cannot look inside this yourself, so whycant "
          "cannot either%s\n", c_dim, c_off);
  printf ("\n  %sTo continue, run the same command as root:%s\n    sudo",
          c_bold, c_off);
  for (int i = 0; i < g_argc; i++)
    {
      putchar (' ');
      put_shell_word (g_argv[i]);
    }
  putchar ('\n');
}

static int
conclude (bool ok)
{
  if (g_json)
    {
      J.allowed = ok;
      return ok ? EXIT_ALLOWED : EXIT_DENIED;
    }
  if (ok)
    printf ("\n  %s=> Allowed.%s\n", c_ok, c_off);
  else
    printf ("\n  %s=> Denied.%s\n", c_bad, c_off);
  return ok ? EXIT_ALLOWED : EXIT_DENIED;
}

/* Print the collected JSON object for one PATH.  */
static void
json_emit (char const *input, unsigned want, bool create,
           struct user const *u, int status)
{
  if (J.steps)
    fflush (J.steps);
  fputs ("{\"user\":", stdout);
  json_str (stdout, u->name);
  printf (",\"uid\":%lu,\"path\":", (unsigned long) u->uid);
  json_str (stdout, input);
  fputs (",\"access\":", stdout);
  if (create)
    fputs ("\"create\"", stdout);
  else
    printf ("\"%s%s%s\"", want & P_R ? "r" : "", want & P_W ? "w" : "",
            want & P_X ? "x" : "");
  printf (",\"result\":\"%s\"",
          status == EXIT_ALLOWED ? "allowed" : status == EXIT_DENIED ? "denied" : "error");
  printf (",\"steps\":[%s]", J.steps_buf ? J.steps_buf : "");
  if (*J.problem)
    {
      fputs (",\"problem\":", stdout);
      json_str (stdout, J.problem);
      fputs (",\"problem_path\":", stdout);
      json_str (stdout, J.problem_path ? J.problem_path : "");
    }
  if (J.sudo_command)
    {
      fputs (",\"sudo_command\":", stdout);
      json_str (stdout, J.sudo_command);
    }
  if (J.error)
    {
      fputs (",\"error\":", stdout);
      json_str (stdout, J.error);
    }
  if (J.has_advice)
    {
      struct advice const *a = &g_advice;
      fputs (",\"why\":", stdout);
      json_str (stdout, a->why);
      if (*a->note)
        {
          fputs (",\"note\":", stdout);
          json_str (stdout, a->note);
        }
      if (a->blocker_fix)
        {
          fputs (",\"fix\":", stdout);
          json_str (stdout, a->blocker_fix);
        }
      if (*a->stop)
        {
          fputs (",\"stop\":", stdout);
          json_str (stdout, a->stop);
        }
      fputs (",\"fixes\":[", stdout);
      for (int i = 0; i < a->nfix; i++)
        {
          fputs (i ? ",{\"command\":" : "{\"command\":", stdout);
          json_str (stdout, a->fix[i].cmd);
          fputs (",\"scope\":", stdout);
          json_str (stdout, a->fix[i].scope);
          putc ('}', stdout);
        }
      putc (']', stdout);
    }
  fputs ("}\n", stdout);
}

/* ---------------------------------------------------------------------
   The walk.
   --------------------------------------------------------------------- */

/* Explain whether U can access INPUT with the bits in WANT, or create
   it when CREATE.  Return an exit status.  */
static int
explain_walk (char const *input, unsigned want, bool create,
              struct user const *u)
{
  /* CUR is always a physical absolute path: symlinks are expanded as
     they are met, which is what makes ".." behave like the kernel's.
     REST holds the components still to be resolved.  */
  char cur[PATH_MAX], rest[PATH_MAX], next[PATH_MAX];
  char target[PATH_MAX], expanded[PATH_MAX];
  char last_ok[PATH_MAX] = "";
  struct stat st;
  int symlinks = 0;

  if (!*input)
    {
      fprintf (stderr, "%s: empty path\n", PROGRAM_NAME);
      return EXIT_TROUBLE;
    }
  if (strlen (input) >= sizeof rest)
    {
      warn_path ("cannot inspect", input, ENAMETOOLONG, NULL);
      return EXIT_TROUBLE;
    }
  strcpy (rest, input);

  if (input[0] == '/')
    strcpy (cur, "/");
  else if (!getcwd (cur, sizeof cur))
    {
      fprintf (stderr, "%s: cannot get working directory: %s\n",
               PROGRAM_NAME, strerror (errno));
      return EXIT_TROUBLE;
    }
  if (lstat (cur, &st) != 0)
    {
      warn_path ("cannot inspect", cur, errno, NULL);
      return EXIT_TROUBLE;
    }

  if (!g_json)
    {
      char action[64];
      describe_want (want, create, action, sizeof action);
      printf ("%sCan %s (uid %lu) %s ", c_bold, u->name,
              (unsigned long) u->uid, action);
      put_escaped (stdout, input);
      printf ("?%s\n\n", c_off);
    }

  for (;;)
    {
      char *p = rest;
      while (*p == '/')
        p++;
      if (!*p)
        break;                  /* CUR is the final object.  */

      size_t len = strcspn (p, "/");
      if (len > NAME_MAX)
        {
          report_problem ("[FAIL]", cur, "next name is longer than NAME_MAX");
          return conclude (false);
        }
      char name[NAME_MAX + 1];
      memcpy (name, p, len);
      name[len] = '\0';

      char const *after = p + len;
      bool slash_follows = *after == '/';
      while (*after == '/')
        after++;
      bool last = !*after;
      memmove (rest, after, strlen (after) + 1);

      /* Looking up any name in a directory -- even "." or ".." --
         needs search (x) permission on that directory.  The same
         directory can come up twice in a row (after "." or a relative
         symlink); one "ok" line for it is enough.  */
      struct verdict v = evaluate (cur, &st, P_X, u);
      if (!v.ok || strcmp (cur, last_ok) != 0)
        report_step (cur, &st, P_X, &v, "pass through");
      if (!v.ok)
        {
          suggest_fixes (cur, &st, &v, u);
          return conclude (false);
        }
      strcpy (last_ok, cur);

      bool dot = strcmp (name, ".") == 0;
      bool dotdot = strcmp (name, "..") == 0;
      if (create && last && (dot || dotdot))
        {
          report_problem ("[FAIL]", input, "already exists");
          return conclude (false);
        }
      if (dot)
        continue;
      if (dotdot)
        {
          to_parent (cur);
          if (lstat (cur, &st) != 0)
            {
              warn_path ("cannot inspect", cur, errno, NULL);
              return EXIT_TROUBLE;
            }
          continue;
        }

      if (!join_path (next, sizeof next, cur, name))
        {
          report_problem ("[FAIL]", cur, "path grows past PATH_MAX");
          return conclude (false);
        }

      struct stat nst;
      if (lstat (next, &nst) != 0)
        {
          int err = errno;
          if (err == EACCES && geteuid () != 0)
            {
              suggest_sudo (next);
              return EXIT_TROUBLE;
            }
          if (err != ENOENT)
            {
              warn_path ("cannot inspect", next, err, NULL);
              return EXIT_TROUBLE;
            }
          if (create && last)
            {
              /* Adding a name to a directory needs write on it.  */
              v = evaluate (cur, &st, P_W, u);
              report_step (cur, &st, P_W, &v, "add entry");
              if (!v.ok)
                suggest_fixes (cur, &st, &v, u);
              return conclude (v.ok);
            }
          report_problem ("[MISS]", next,
                          last ? "does not exist"
                          : "does not exist (a parent directory is missing)");
          return conclude (false);
        }

      if (create && last)
        {
          report_problem ("[FAIL]", next, "already exists");
          return conclude (false);
        }

      if (S_ISLNK (nst.st_mode))
        {
          if (++symlinks > MAX_SYMLINKS)
            {
              report_problem ("[FAIL]", next,
                              "too many symbolic links (a loop?)");
              return conclude (false);
            }
          ssize_t n = readlink (next, target, sizeof target);
          if (n < 0)
            {
              warn_path ("cannot read link", next, errno, NULL);
              return EXIT_TROUBLE;
            }
          if ((size_t) n >= sizeof target)
            {
              report_problem ("[FAIL]", next, "link target too long");
              return conclude (false);
            }
          target[n] = '\0';

          report_link (next, target);

          /* Splice the target in front of what is left.  A slash after
             the link name ("link/") still demands a directory.  */
          int len2;
          if (*rest)
            len2 = snprintf (expanded, sizeof expanded, "%s/%s", target, rest);
          else
            len2 = snprintf (expanded, sizeof expanded, "%s%s", target,
                             slash_follows ? "/" : "");
          if (len2 < 0 || (size_t) len2 >= sizeof expanded)
            {
              report_problem ("[FAIL]", next, "path grows past PATH_MAX");
              return conclude (false);
            }
          strcpy (rest, expanded);

          if (target[0] == '/')
            {
              strcpy (cur, "/");
              if (lstat (cur, &st) != 0)
                {
                  warn_path ("cannot inspect", cur, errno, NULL);
                  return EXIT_TROUBLE;
                }
            }
          continue;
        }

      if ((!last || slash_follows) && !S_ISDIR (nst.st_mode))
        {
          report_problem ("[FAIL]", next,
                          "is not a directory, but the path goes on");
          return conclude (false);
        }

      strcpy (cur, next);
      st = nst;
    }

  /* CUR names the object itself (a path like "/" or "dir/." ends here).  */
  if (create)
    {
      report_problem ("[FAIL]", input, "already exists");
      return conclude (false);
    }
  struct verdict v = evaluate (cur, &st, want, u);
  report_step (cur, &st, want, &v, "access");
  if (!v.ok)
    suggest_fixes (cur, &st, &v, u);
  return conclude (v.ok);
}

static int
explain (char const *input, unsigned want, bool create, struct user const *u)
{
  if (!g_json)
    return explain_walk (input, want, create, u);
  json_reset ();
  advice_free (&g_advice);
  free (g_last_error);
  g_last_error = NULL;
  int status = explain_walk (input, want, create, u);
  if (status == EXIT_TROUBLE && g_last_error && !J.error)
    J.error = strdup (g_last_error);
  json_emit (input, want, create, u, status);
  return status;
}

/* ---------------------------------------------------------------------
   Command line.
   --------------------------------------------------------------------- */

static void
usage (int status)
{
  if (status != EXIT_ALLOWED)
    {
      fprintf (stderr, "Try '%s --help' for more information.\n",
               PROGRAM_NAME);
      exit (status);
    }
  printf ("\
Usage: %s [OPTION]... [PATH]...\n\
Explain, step by step, whether a user can access PATH, and if not,\n\
exactly which directory or file blocks it and the narrowest fix.\n\
With no PATH, check the current folder.  Under sudo, check for the user\n\
who ran sudo (use -u root to check root itself).\n\
\n\
  -u, --user=USER   check for USER (name or uid); default: you\n\
  -r, --read        check read access (the default)\n\
  -w, --write       check write access\n\
  -x, --exec        check execute access (search, for a directory)\n\
  -c, --create      check whether USER could create PATH (mkdir, touch)\n\
      --json        print one JSON object per PATH, for scripts\n\
  -h, --help        display this help and exit\n\
  -V, --version     output version information and exit\n\
\n\
Exit status: 0 if allowed, 1 if denied, 2 if trouble.\n\
\n\
Examples:\n\
  %s -u www-data /var/www/site/index.html\n\
  %s -u alice -c /srv/shared/reports/2026\n\
  sudo %s -u postgres -rw /etc/postgresql/16/main/pg_hba.conf\n",
          PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME);
  exit (status);
}

int
main (int argc, char **argv)
{
  static struct option const longopts[] = {
    { "user", required_argument, NULL, 'u' },
    { "read", no_argument, NULL, 'r' },
    { "write", no_argument, NULL, 'w' },
    { "exec", no_argument, NULL, 'x' },
    { "create", no_argument, NULL, 'c' },
    { "json", no_argument, NULL, 'J' },
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, 'V' },
    { NULL, 0, NULL, 0 }
  };

  g_argc = argc;
  g_argv = argv;

  char const *user_spec = NULL;
  unsigned want = 0;
  bool create = false;
  int c;

  while ((c = getopt_long (argc, argv, "u:rwxchV", longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'u':
          user_spec = optarg;
          break;
        case 'r':
          want |= P_R;
          break;
        case 'w':
          want |= P_W;
          break;
        case 'x':
          want |= P_X;
          break;
        case 'c':
          create = true;
          break;
        case 'J':
          g_json = true;
          break;
        case 'h':
          usage (EXIT_ALLOWED);
          break;
        case 'V':
          puts (PROGRAM_NAME " " PROGRAM_VERSION);
          return EXIT_ALLOWED;
        default:
          usage (EXIT_TROUBLE);
        }
    }

  if (create && want)
    {
      fprintf (stderr, "%s: --create cannot be combined with -r, -w or -x\n",
               PROGRAM_NAME);
      usage (EXIT_TROUBLE);
    }
  if (!want)
    want = P_R;

  setup_colours ();

  struct user u;
  if (!load_user (user_spec, &u))
    return EXIT_TROUBLE;

  /* No PATH given: like ls, look at the current folder, shown by its
     full name so every folder on the way to it is checked.  */
  char cwd[PATH_MAX];
  char *here[] = { getcwd (cwd, sizeof cwd) ? cwd : (char *) ".", NULL };
  char **paths = argv + optind;
  int npaths = argc - optind;
  if (npaths == 0)
    {
      paths = here;
      npaths = 1;
    }

  /* Worst result wins: trouble (2) > denied (1) > allowed (0).  */
  int status = EXIT_ALLOWED;
  for (int i = 0; i < npaths; i++)
    {
      if (i > 0 && !g_json)
        putchar ('\n');
      int s = explain (paths[i], want, create, &u);
      if (s > status)
        status = s;
    }

  free (u.groups);
  if (g_json)
    {
      json_reset ();
      if (J.steps)
        fclose (J.steps);
      free (J.steps_buf);
      J.steps = NULL;
      J.steps_buf = NULL;
      advice_free (&g_advice);
    }
  free (g_last_error);

  /* Like coreutils' close_stdout: a full disk or closed pipe must not
     be reported as success.  */
  if (fflush (stdout) != 0 || ferror (stdout))
    {
      fprintf (stderr, "%s: write error: %s\n", PROGRAM_NAME,
               strerror (errno));
      return EXIT_TROUBLE;
    }
  return status;
}
