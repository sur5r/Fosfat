/*
 * FOS fosmount: FUSE extension in read-only for Smaky file system
 * Copyright (C) 2006-2010 Mathieu Schroeter <mathieu@schroetersa.ch>
 *
 * Thanks to Pierre Arnaud for his help and the documentation
 *    And to Epsitec SA for the Smaky computers
 *
 * This file is part of Fosfat.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>     /* strcmp strncmp strstr strlen strdup memcpy memset */
#include <time.h>       /* mktime */
#include <fuse.h>
#include <getopt.h>

#include "fosfat.h"
#include "fosgra.h"

/* Rights */
#define FOS_DIR             0555
#define FOS_FILE            0444

#define HELP_TEXT \
"FUSE extension for a read-only access on Smaky FOS. fosfat-" LIBFOSFAT_VERSION_STR "\n\n" \
"Usage: fosmount [options] device mountpoint\n\n" \
" -h --help             this help\n" \
" -v --version          version\n" \
" -a --harddisk         force an hard disk (default autodetect)\n" \
" -f --floppydisk       force a floppy disk (default autodetect)\n" \
" -l --fos-logger       that will turn on the FOS logger\n" \
" -d --fuse-debugger    that will turn on the FUSE debugger\n" \
" -i --image-bmp        convert on the fly .IMAGE and .COLOR to .BMP\n" \
" device                /dev/fd0 : floppy disk\n" \
"                       /dev/sda : hard disk, etc\n" \
" mountpoint            for example, /mnt/smaky\n" \
"\nPlease, report bugs to <mathieu@schroetersa.ch>.\n"

#define VERSION_TEXT "fosmount-" LIBFOSFAT_VERSION_STR "\n"


static fosfat_t *fosfat;

#define FOSGRAID "@"

static int g_bmp = 0;


static int
count_nb_dec (int dec)
{
  int size = 1;

  while (dec /= 10)
    size++;

  return size;
}

static char *
trim_fosname (const char *path)
{
  char *it, res[256];

  snprintf (res, sizeof (res), "%s", path);
  it = strstr (res, "." FOSGRAID ".");
  if (it)
    *it = '\0';

  return strdup (res);
}

// Structure de l'en-tête du fichier BMP
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} __attribute__((packed)) BITMAPFILEHEADER;

// Structure de l'en-tête DIB (Device Independent Bitmap)
typedef struct {
    uint32_t biSize;
    int32_t biWidth;
    int32_t biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t biXPelsPerMeter;
    int32_t biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} __attribute__((packed)) BITMAPINFOHEADER;

// Structure de la palette de couleurs pour le format 1 bit par pixel
typedef struct {
    uint8_t rgbBlue;
    uint8_t rgbGreen;
    uint8_t rgbRed;
    uint8_t rgbReserved;
} __attribute__((packed)) RGBQUAD;

// Fonction pour créer un buffer BMP monochrome
static void create_bmp1_buffer(const uint8_t *input, int width, int height, uint8_t **output, size_t *output_size) {
    int bytes_per_row = (width + 7) / 8; // Nombre de bytes par ligne
    int padded_bytes_per_row = (bytes_per_row + 3) / 4 * 4; // Alignement sur 4 bytes

    // Calcul de la taille totale du buffer BMP
    size_t bmp_size = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 2 * sizeof(RGBQUAD) + padded_bytes_per_row * height;

    // Allocation du buffer BMP
    *output = (uint8_t *)malloc(bmp_size);
    if (*output == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    // Remplissage de l'en-tête du fichier BMP
    BITMAPFILEHEADER *bfh = (BITMAPFILEHEADER *)(*output);
    bfh->bfType = 0x4D42; // 'BM'
    bfh->bfSize = bmp_size;
    bfh->bfReserved1 = 0;
    bfh->bfReserved2 = 0;
    bfh->bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 2 * sizeof(RGBQUAD);

    // Remplissage de l'en-tête DIB
    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)(*output + sizeof(BITMAPFILEHEADER));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = width;
    bih->biHeight = -height;
    bih->biPlanes = 1;
    bih->biBitCount = 1;
    bih->biCompression = 0;
    bih->biSizeImage = 0;
    bih->biXPelsPerMeter = 0;
    bih->biYPelsPerMeter = 0;
    bih->biClrUsed = 0;
    bih->biClrImportant = 0;

    // Remplissage de la palette de couleurs
    RGBQUAD *palette = (RGBQUAD *)(*output + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    palette[0].rgbBlue = 255;
    palette[0].rgbGreen = 255;
    palette[0].rgbRed = 255;
    palette[0].rgbReserved = 0;
    palette[1].rgbBlue = 0;
    palette[1].rgbGreen = 0;
    palette[1].rgbRed = 0;
    palette[1].rgbReserved = 0;

    // Copie des données de l'image
    uint8_t *image_data = *output + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 2 * sizeof(RGBQUAD);
    for (int y = 0; y < height; y++) {
        memcpy(image_data + y * padded_bytes_per_row, input + y * bytes_per_row, bytes_per_row);
        memset(image_data + y * padded_bytes_per_row + bytes_per_row, 0, padded_bytes_per_row - bytes_per_row);
    }

    *output_size = bmp_size;
}

// Fonction pour créer un buffer BMP
static void create_bmp4_buffer(const uint8_t *input, const uint32_t *pal, int width, int height, uint8_t **output, size_t *output_size) {
    int bytes_per_row = (width + 1) / 2; // Nombre de bytes par ligne
    int image_size = bytes_per_row * height; // Taille de l'image en bytes
    int palette_size = 16 * sizeof(RGBQUAD); // Taille de la palette
    int header_size = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + palette_size;
    int total_size = header_size + image_size; // Taille totale du fichier BMP

    // Allouer de la mémoire pour le buffer de sortie
    *output = (uint8_t *)malloc(total_size);
    *output_size = total_size;

    // Remplir l'en-tête de fichier BMP
    BITMAPFILEHEADER *file_header = (BITMAPFILEHEADER *)(*output);
    file_header->bfType = 0x4D42; // 'BM'
    file_header->bfSize = total_size;
    file_header->bfReserved1 = 0;
    file_header->bfReserved2 = 0;
    file_header->bfOffBits = header_size;

    // Remplir l'en-tête DIB
    BITMAPINFOHEADER *info_header = (BITMAPINFOHEADER *)(*output + sizeof(BITMAPFILEHEADER));
    info_header->biSize = sizeof(BITMAPINFOHEADER);
    info_header->biWidth = width;
    info_header->biHeight = -height;
    info_header->biPlanes = 1;
    info_header->biBitCount = 4;
    info_header->biCompression = 0;
    info_header->biSizeImage = image_size;
    info_header->biXPelsPerMeter = 0;
    info_header->biYPelsPerMeter = 0;
    info_header->biClrUsed = 16;
    info_header->biClrImportant = 0;

    // Remplir la palette
    RGBQUAD *palette = (RGBQUAD *)(*output + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    for (int i = 0; i < 16; i++) {
        palette[i].rgbBlue  = pal[i] >> 16 & 0xFF;
        palette[i].rgbGreen = pal[i] >>  8 & 0xFF;
        palette[i].rgbRed   = pal[i] >>  0 & 0xFF;
        palette[i].rgbReserved = 0;
    }

    // Copier les données de l'image
    uint8_t *image_data = *output + header_size;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x += 2) {
            uint8_t byte = input[y * bytes_per_row + x / 2];
            image_data[y * bytes_per_row + x / 2] = byte;
        }
    }
}

static const uint8_t *
load_image (const char *path, size_t *size)
{
  uint8_t bpp = 0;
  uint16_t w = 0, h = 0;
  size_t raw_size = 0;
  static uint8_t *img_buffer = NULL;
  static uint8_t *out_buffer = NULL;
  static size_t out_length = 0;

  *size = 0;

  fosgra_get_info (fosfat, path, &w, &h, &bpp);

  /* Load the image in the cache */
  raw_size = bpp == 1 ? w * h / 8 : /* bpp == 4 */ w * h / 2;
  img_buffer = fosgra_get_buffer (fosfat, path, 0, raw_size);

  if (bpp == 1)
    create_bmp1_buffer (img_buffer, w, h, &out_buffer, &out_length);

  if (bpp == 4)
  {
    uint32_t pal[16];

    for (int idx = 0; idx < 16; ++idx)
      pal[idx] = fosgra_color_get (fosfat, path, idx);

    create_bmp4_buffer (img_buffer, pal, w, h, &out_buffer, &out_length);
  }

  *size = out_length;
  return out_buffer;
}

static size_t
get_filesize (fosfat_file_t *file, const char *path)
{
  char *it = NULL;
  size_t image_size = 0;

  if (g_bmp)
    it = strstr (file->name, ".image\0") || strstr (file->name, ".color\0");

  if (!it || file->att.isdir || file->att.islink || file->att.isencoded
      || !fosgra_is_image (fosfat, path))
    return file->size;

  load_image (path, &image_size);
  return image_size;
}

static uint8_t *
get_buffer (fosfat_file_t *file, const char *path, off_t offset, size_t size)
{
  char *it = NULL;
  uint8_t *dec;
  const uint8_t *image_buffer;

  if (g_bmp)
    it = strstr (file->name, ".image\0") || strstr (file->name, ".color\0");

  if (!it || file->att.isdir || file->att.islink || file->att.isencoded
      || !fosgra_is_image (fosfat, path))
    return fosfat_get_buffer (fosfat, path, offset, size);

  size_t image_size = 0;
  image_buffer = load_image (path, &image_size);

  dec = calloc (1, size);
  memcpy (dec, image_buffer + offset, size);
  return dec;
}

/*
 * Convert 'fosfat_file_t' to 'struct stat'.
 */
static struct stat *
in_stat (fosfat_file_t *file, const char *path)
{
  struct stat *st;
  struct tm time;

  st = calloc (1, sizeof (struct stat));
  if (!st)
    return NULL;

  memset (&time, 0, sizeof (time));

  /* Directory, symlink or file */
  if (file->att.isdir)
  {
    st->st_mode = S_IFDIR | FOS_DIR;
    st->st_nlink = 2;
  }
  else if (file->att.islink)
  {
    st->st_mode = S_IFLNK | FOS_DIR;
    st->st_nlink = 2;
  }
  else
  {
    st->st_mode = S_IFREG | FOS_FILE;
    st->st_nlink = 1;
  }

  /* Size */
  st->st_size = get_filesize (file, path);

  /* Time */
  time.tm_year = file->time_r.year - 1900;
  time.tm_mon  = file->time_r.month - 1;
  time.tm_mday = file->time_r.day;
  time.tm_hour = file->time_r.hour;
  time.tm_min  = file->time_r.minute;
  time.tm_sec  = file->time_r.second;
  st->st_atime = mktime(&time);
  time.tm_year = file->time_w.year - 1900;
  time.tm_mon  = file->time_w.month - 1;
  time.tm_mday = file->time_w.day;
  time.tm_hour = file->time_w.hour;
  time.tm_min  = file->time_w.minute;
  time.tm_sec  = file->time_w.second;
  st->st_mtime = mktime(&time);
  time.tm_year = file->time_c.year - 1900;
  time.tm_mon  = file->time_c.month - 1;
  time.tm_mday = file->time_c.day;
  time.tm_hour = file->time_c.hour;
  time.tm_min  = file->time_c.minute;
  time.tm_sec  = file->time_c.second;
  st->st_ctime = mktime(&time);

  return st;
}

/*
 * Get the file stat from a path an return as struct stat.
 */
static struct stat *
get_stat (const char *path)
{
  char *location;
  struct stat *st = NULL;
  fosfat_file_t *file;

  location = trim_fosname (path);

  file = fosfat_get_stat (fosfat, location);
  if (file)
  {
    st = in_stat (file, location);
    free (file);
  }

  if (location)
    free (location);

  return st;
}

/*
 * FUSE : read the target of a symlink.
 *
 * path         (foo/bar)
 * dst          target
 * size         max length
 * return 0 for success
 */
static int
fos_readlink (const char *path, char *dst, size_t size)
{
  int res = -ENOENT;
  char *link, *location;

  location = trim_fosname (path);

  link = fosfat_symlink (fosfat, location);

  if (strlen (link) < size)
  {
    memcpy (dst, link, strlen (link) + 1);
    res = 0;
  }

  if (link)
    free (link);
  if (location)
    free (location);

  return res;
}

/*
 * FUSE : get attributes of a file.
 *
 * path         (foo/bar)
 * stbuf        attributes
 * return 0 for success
 */
static int
fos_getattr (const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
  int ret = 0;
  char *location;
  struct stat *st;

  (void) fi;

  /* Root directory */
  if (!strcmp (path, "/"))
    location = strdup ("/sys_list");
  else
    location = trim_fosname (path);

  /* Get file stats */
  st = get_stat (location);
  if (st)
  {
    memcpy (stbuf, st, sizeof (*stbuf));
    free (st);
  }
  else
    ret = -ENOENT;

  if (location)
    free (location);

  return ret;
}

/*
 * FUSE : read a directory.
 *
 * path         (foo/bar)
 * buf          buffer for filler
 * filler       function for put each entry
 * offset       not used
 * fi           not used
 * return 0 for success
 */
static int
fos_readdir (const char *path, void *buf, fuse_fill_dir_t filler,
             off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
  int ret = -ENOENT;
  char *location;
  fosfat_file_t *files, *first_file;

  (void) offset;
  (void) fi;
  (void) flags;

  location = trim_fosname (path);

  /* First entries */
  filler (buf, "..", NULL, 0, 0);

  /* Files and directories */
  files = fosfat_list_dir (fosfat, location);
  if (!files)
    goto out;

  first_file = files;

  do
  {
    char _path[256];
    struct stat *st;
    char *name;

    snprintf (_path, sizeof (_path), "%s/%s", location, files->name);
    st = in_stat (files, _path);

    /* add identification for .IMAGE and .COLOR translated to BMP */
    if (g_bmp
        && (   strstr (files->name, ".image\0")
            || strstr (files->name, ".color\0"))
        && fosgra_is_image (fosfat, _path))
    {
      name = calloc (1, strlen (files->name) + strlen (FOSGRAID) + 6);
      sprintf (name, "%s." FOSGRAID ".bmp", files->name);
    }
    else
      name = strdup (files->name);

    if (strstr (name, ".dir"))
      *(name + strlen (name) - 4) = '\0';

    /* Add entry in the file list */
    filler (buf, name, st, 0, 0);
    free (st);
    free (name);
  }
  while ((files = files->next_file));

  fosfat_free_listdir (first_file);
  ret = 0;

 out:
  if (location)
    free (location);

  return ret;
}

/*
 * FUSE : test if a file can be opened.
 *
 * path         (foo/bar)
 * fi           flags
 * return 0 for success
 */
static int
fos_open (const char *path, struct fuse_file_info *fi)
{
  int ret = 0;
  char *location;

  location = trim_fosname (path);

  if ((fi->flags & 3) != O_RDONLY)
    ret = -EACCES;
  else if (!fosfat_isopenexm (fosfat, location))
    ret = -ENOENT;

  if (location)
    free (location);

  return ret;
}

/*
 * FUSE : read the data of a file.
 *
 * path         (foo/bar)
 * buf          buffer for put the data
 * size         size in bytes
 * offset       offset in bytes
 * fi           not used
 * return the size
 */
static int
fos_read (const char *path, char *buf, size_t size,
          off_t offset, struct fuse_file_info *fi)
{
  int res = -ENOENT;
  int length;
  char *location;
  uint8_t *buf_tmp;
  fosfat_file_t *file = NULL;

  (void) fi;

  location = trim_fosname (path);

  /* Get the stats and test if it is a file */
  file = fosfat_get_stat (fosfat, location);
  if (!file)
    goto out;

  if (file->att.isdir)
    goto out;

  length = get_filesize (file, location);

  if (offset < length)
  {
    /* Fix the size in function of the offset */
    if (offset + (signed) size > length)
      size = length - offset;

    /* Read the data */
    buf_tmp = get_buffer (file, location, offset, size);

    /* Copy the data for FUSE */
    if (buf_tmp)
    {
      memcpy (buf, buf_tmp, size);
      free (buf_tmp);
      res = size;
    }
  }
  else
    res = 0;

 out:
  if (file)
    free (file);
  if (location)
    free (location);

  return res;
}

/* Print help. */
void
print_info (void)
{
  printf (HELP_TEXT);
}

/* Print version. */
void
print_version (void)
{
  printf (VERSION_TEXT);
}

/* FUSE implemented functions */
static struct fuse_operations fosfat_oper = {
  .getattr  = fos_getattr,
  .readdir  = fos_readdir,
  .open     = fos_open,
  .read     = fos_read,
  .readlink = fos_readlink,
};

int
main (int argc, char **argv)
{
  int i;
  int next_option;
  int res = 0, fusedebug = 0, foslog = 0;
  char *device;
  char **arg;
  fosfat_disk_t type = FOSFAT_AD;

  const char *const short_options = "adfhliv";

  const struct option long_options[] = {
    { "harddisk",      no_argument, NULL, 'a' },
    { "fuse-debugger", no_argument, NULL, 'd' },
    { "floppydisk",    no_argument, NULL, 'f' },
    { "help",          no_argument, NULL, 'h' },
    { "fos-logger",    no_argument, NULL, 'l' },
    { "image-bmp",     no_argument, NULL, 'i' },
    { "version",       no_argument, NULL, 'v' },
    { NULL,            0,           NULL,  0  }
  };

  /* check options */
  do
  {
    next_option = getopt_long (argc, argv, short_options, long_options, NULL);
    switch (next_option)
    {
    default :           /* unknown */
    case '?':           /* invalid option */
    case 'h':           /* -h or --help */
      print_info ();
      return -1;
    case 'v':           /* -v or --version */
      print_version ();
      return -1;
    case 'f':           /* -f or --floppydisk */
      type = FOSFAT_FD;
      break ;
    case 'a':           /* -a or --harddisk */
      type = FOSFAT_HD;
      break ;
    case 'd':           /* -d or --fuse-debugger */
      fusedebug = 1;
      break ;
    case 'l':           /* -l or --fos-debugger */
      foslog = 1;
      fosfat_logger (1);
      break;
    case 'i':           /* -i or --image-bmp */
      g_bmp = 1;
      break ;
    case -1:            /* end */
      break ;
    }
  }
  while (next_option != -1);

  if (argc < optind + 2)
  {
    print_info ();
    return -1;
  }

  /* table for fuse */
  arg = malloc (sizeof (char *) * (3 + fusedebug + foslog));
  if (arg)
  {
    arg[0] = strdup (argv[0]);

    if (fusedebug)
      arg[fusedebug] = strdup ("-d");
    if (foslog)
      arg[fusedebug + foslog] = strdup ("-f");

    device = strdup (argv[optind]);
    arg[1 + fusedebug + foslog] = strdup (argv[optind + 1]);

    /* FUSE must be used as single-thread */
    arg[2 + fusedebug + foslog] = strdup ("-s");
  }
  else
    return -1;

  /* Open the floppy disk (or hard disk) */
  fosfat = fosfat_open (device, type, 0);
  if (!fosfat)
  {
    fprintf (stderr, "Could not open %s for mounting!\n", device);
    res = -1;
  }
  else
  {
    /* FUSE */
    res = fuse_main (3 + fusedebug + foslog, arg, &fosfat_oper, NULL);

    /* Close the device */
    fosfat_close (fosfat);
  }

  for (i = 0; i < 3 + fusedebug + foslog; i++)
    free (*(arg + i));
  free (arg);

  /* Free */
  free (device);

  return res;
}
