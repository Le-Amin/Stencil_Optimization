/*
 * Stencil 5 points pondéré - version optimisée et parallélisée (OpenMP).
 *
 * Optimisations appliquées par rapport à la version d'origine :
 *  1. Parcours mémoire contigu : la boucle interne parcourt j (indice contigu
 *     dans le stockage image[j + i*height]) au lieu de i (accès à pas = height).
 *  2. Grille avec halo de zéros : plus aucun test de bord dans la boucle
 *     interne, ce qui la rend entièrement vectorisable (SIMD).
 *  3. Simple précision (float) par défaut : divise par deux le trafic mémoire.
 *     Compiler avec -DUSE_DOUBLE pour rester en double précision.
 *  4. Constantes précalculées (0.6 et 0.1), pointeurs restrict, allocation
 *     alignée sur 64 octets.
 *  5. Blocage temporel (« wavefront ») : T balayages successifs du stencil
 *     sont fusionnés en un seul passage sur la grille. Les niveaux
 *     intermédiaires ne vivent que dans un anneau de 3 colonnes par niveau.
 *     La grille n'est lue/écrite en mémoire qu'une fois tous les T balayages.
 *  6. Blocage spatial en j : les colonnes sont découpées en bandes de BJ
 *     lignes (avec un halo recalculé de T lignes) pour que l'anneau tienne
 *     dans le cache L2, quelle que soit la taille du problème.
 *  7. Parallélisation OpenMP : la grille est découpée en px x py blocs
 *     (colonnes x lignes, blocs les plus carrés possible), un par thread ;
 *     les bords des blocs sont recalculés de façon redondante,
 *     donc aucune synchronisation n'est nécessaire au sein d'un passage.
 *     Initialisation parallèle (« first touch ») pour le placement NUMA.
 *
 * Variables d'environnement (optionnelles) :
 *   OMP_NUM_THREADS  nombre de threads
 *   OMP_PROC_BIND / OMP_PLACES  placement des threads (conseillé : close / cores)
 *   STENCIL_T        balayages fusionnés par passage (défaut 32, 1 = aucun)
 *   STENCIL_BJ       hauteur des bandes en j (défaut : calculée d'après le L2)
 *   STENCIL_L2       taille du cache L2 en octets (si la détection échoue)
 *   STENCIL_PX       nombre de blocs de colonnes (défaut : automatique)
 */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef _OPENMP
#include <omp.h>
#endif

// Define output file name
#define OUTPUT_FILE "stencil.pgm"

#ifdef USE_DOUBLE
typedef double real;
#define C_CENTER (3.0 / 5.0)
#define C_NEIGH  (0.5 / 5.0)
#else
typedef float real;
#define C_CENTER (3.0f / 5.0f)
#define C_NEIGH  (0.5f / 5.0f)
#endif

#define ALIGN 64
#define DEFAULT_T 32
#define DEFAULT_L2 (512 * 1024)

static void* xalloc(size_t bytes);
static long env_long(const char* name, long def);
static void init_image(const int nx, const int ny, const int height,
                       real* image, real* tmp_image);
static void stencil_pass(const int nx, const int ny, const int height,
                         const int T, const int bj, const int px,
                         const int py, const real* restrict src,
                         real* restrict dst);
static void output_image(const char* file_name, const int nx, const int ny,
                         const int height, const real* image);
static double wtime(void);

int main(int argc, char* argv[])
{
  // Check usage
  if (argc != 4) {
    fprintf(stderr, "Usage: %s nx ny niters\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  // Initiliase problem dimensions from command line arguments
  const int nx = atoi(argv[1]);
  const int ny = atoi(argv[2]);
  const int niters = atoi(argv[3]);
  if (nx <= 0 || ny <= 0 || niters < 0) {
    fprintf(stderr, "Error: nx and ny must be > 0 and niters >= 0\n");
    exit(EXIT_FAILURE);
  }

#ifdef _OPENMP
  const int nthreads = omp_get_max_threads();
#else
  const int nthreads = 1;
#endif

  // 2D split of the grid between threads: px blocks of columns times py
  // blocks of rows (px * py = nthreads). The most square blocks are chosen,
  // because the redundant halo work grows with the block perimeter.
  int px = nthreads, py = 1;
  {
    double best = -1.0;
    for (int p = 1; p <= nthreads; ++p) {
      if (nthreads % p) continue;
      const double w = (double)nx / p, h = (double)ny / (nthreads / p);
      const double cost = 1.0 / w + 1.0 / h;
      if (best < 0 || cost < best) {
        best = cost;
        px = p;
      }
    }
    py = nthreads / px;
  }
  const long px_env = env_long("STENCIL_PX", 0);
  if (px_env > 0 && nthreads % px_env == 0) {
    px = (int)px_env;
    py = nthreads / px;
  }

  // Temporal blocking depth. Each thread recomputes about T columns/rows on
  // each side of its block, so T is capped to keep that overhead small.
  int T = (int)env_long("STENCIL_T", DEFAULT_T);
  if (T < 1) T = 1;
  if (!getenv("STENCIL_T")) {
    const int w = nx / px, h = ny / py;
    const int m = w < h ? w : h;
    const int cap = m / 8 > 1 ? m / 8 : 1;
    if (T > cap) T = cap;
  }

  // Strip height: the ring of 3*(T-1) columns should use at most half of L2.
  long l2 = 0;
#ifdef _SC_LEVEL2_CACHE_SIZE
  l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);
#endif
  if (l2 <= 0) l2 = DEFAULT_L2;
  l2 = env_long("STENCIL_L2", l2);
  long bj = ny;
  if (T > 1) {
    bj = l2 / 2 / (3L * (T - 1) * (long)sizeof(real)) - 2L * T - 2;
    if (bj < 4L * T) bj = 4L * T;
  }
  bj = env_long("STENCIL_BJ", bj);
  if (bj < 1) bj = 1;
  if (bj > ny) bj = ny;

  // Padded grid: one column of zeros on each side (i) and one row of zeros
  // on each side (j). The height is rounded up so that every column starts
  // on a 64-byte boundary.
  const int per_line = ALIGN / (int)sizeof(real);
  const int height = ((ny + 2 + per_line - 1) / per_line) * per_line;
  const size_t n = (size_t)height * (size_t)(nx + 2);

  real* image = xalloc(sizeof(real) * n);
  real* tmp_image = xalloc(sizeof(real) * n);

  // Set the input image
  init_image(nx, ny, height, image, tmp_image);

  // Two stencil sweeps per iteration, fused T at a time.
  const long nsweeps = 2L * niters;

  // Call the stencil kernel
  double tic = wtime();
  for (long done = 0; done < nsweeps;) {
    const int t = (nsweeps - done < T) ? (int)(nsweeps - done) : T;
    stencil_pass(nx, ny, height, t, (int)bj, px, py, image, tmp_image);
    real* swap = image;
    image = tmp_image;
    tmp_image = swap;
    done += t;
  }
  double toc = wtime();

  // Output
  printf("------------------------------------\n");
  printf(" runtime: %lf s\n", toc - tic);
  printf("------------------------------------\n");
  fprintf(stderr, " threads=%d (%dx%d) T=%d BJ=%ld precision=%s\n", nthreads,
          px, py, T, bj, sizeof(real) == 4 ? "float" : "double");

  output_image(OUTPUT_FILE, nx, ny, height, image);
  free(image);
  free(tmp_image);
  return EXIT_SUCCESS;
}

static void* xalloc(size_t bytes)
{
  void* p = NULL;
  bytes = ((bytes + ALIGN - 1) / ALIGN) * ALIGN;
  if (posix_memalign(&p, ALIGN, bytes) != 0) {
    fprintf(stderr, "Error: could not allocate %zu bytes\n", bytes);
    exit(EXIT_FAILURE);
  }
  return p;
}

static long env_long(const char* name, long def)
{
  const char* s = getenv(name);
  return (s && *s) ? atol(s) : def;
}

// n output values of one column: out = 0.6*c + 0.1*(l + r + up + down).
// c[-1] and c[n] must be readable (neighbour rows or zero halo).
static inline void stencil_column(const int n, const real* restrict l,
                                  const real* restrict c,
                                  const real* restrict r, real* restrict out)
{
#pragma omp simd
  for (int k = 0; k < n; ++k) {
    out[k] = C_CENTER * c[k] + C_NEIGH * (l[k] + r[k] + c[k - 1] + c[k + 1]);
  }
}

// Apply T consecutive stencil sweeps: dst = S^T(src).
//
// Each thread owns the columns [a, b) and the rows [ya, yb) of a px x py split
// of the grid. For each strip of rows [j0, j1) of its block, it runs
// a wavefront over the columns: at step x, level lev (1..T) computes column
// x - (lev - 1) from three columns of level lev - 1. Level 0 is src, level T
// is dst, levels 1..T-1 live in a private ring of 3 columns per level. Level
// lev is needed on columns [a - (T - lev), b - 1 + (T - lev)] and rows
// [j0 - (T - lev), j1 - 1 + (T - lev)], clipped to the domain: everything
// outside the domain is zero, as in the original code.
static void stencil_pass(const int nx, const int ny, const int height,
                         const int T, const int bj, const int px,
                         const int py, const real* restrict src,
                         real* restrict dst)
{
#pragma omp parallel
  {
#ifdef _OPENMP
    const int nth = omp_get_num_threads();
    const int tid = omp_get_thread_num();
#else
    const int nth = 1;
    const int tid = 0;
#endif
    // Fall back to a 1D split if the team size is not px * py.
    const int qx = (px * py == nth) ? px : nth;
    const int qy = (px * py == nth) ? py : 1;
    const int tx = tid / qy, ty = tid % qy;
    const int a = (int)((long)nx * tx / qx);
    const int b = (int)((long)nx * (tx + 1) / qx);
    const int ya = (int)((long)ny * ty / qy);
    const int yb = (int)((long)ny * (ty + 1) / qy);

    if (a < b && ya < yb) {
      // Ring column index k stores the absolute row ring_base + k.
      const int nlev = T - 1;
      const int hs_raw = (bj < yb - ya ? bj : yb - ya) + 2 * T + 2;
      const int per_line = ALIGN / (int)sizeof(real);
      const size_t hs =
          (size_t)(((hs_raw + per_line - 1) / per_line) * per_line);
      const size_t ring_len = hs * (size_t)(nlev > 0 ? 3 * nlev : 1);
      real* ring = xalloc(sizeof(real) * ring_len);
      // Columns -1 and nx of src are zero halos: used as zero columns.
      const real* zero_l = src;
      const real* zero_r = src + (size_t)(nx + 1) * height;

      for (int j0 = ya; j0 < yb; j0 += bj) {
        const int j1 = (j0 + bj < yb) ? j0 + bj : yb;
        const int ring_base = j0 - T - 1;
        if (nlev > 0) memset(ring, 0, sizeof(real) * ring_len);

        for (int x = a - T + 1; x <= b + T - 2; ++x) {
          for (int lev = 1; lev <= T; ++lev) {
            const int c = x - (lev - 1);
            const int halo = T - lev;
            if (c < a - halo || c > b - 1 + halo || c < 0 || c >= nx) continue;

            int jlo = j0 - halo;
            int jhi = j1 - 1 + halo;
            if (jlo < 0) jlo = 0;
            if (jhi > ny - 1) jhi = ny - 1;

            // Three input columns of level lev - 1, pointing at row jlo.
            const real* in[3];
            for (int d = 0; d < 3; ++d) {
              const int cc = c - 1 + d;
              if (lev == 1 || cc < 0 || cc >= nx) {
                const real* base = (lev == 1) ? src + (size_t)(cc + 1) * height
                                   : (cc < 0) ? zero_l
                                              : zero_r;
                in[d] = base + (jlo + 1);
              } else {
                in[d] = ring + hs * (size_t)(3 * (lev - 2) + cc % 3) +
                        (jlo - ring_base);
              }
            }

            real* out = (lev == T)
                            ? dst + (size_t)(c + 1) * height + (jlo + 1)
                            : ring + hs * (size_t)(3 * (lev - 1) + c % 3) +
                                  (jlo - ring_base);
            stencil_column(jhi - jlo + 1, in[0], in[1], in[2], out);
          }
        }
      }
      free(ring);
    }
  }
}

// Create the input image (parallel first touch, same column split as the
// stencil so that pages land on the NUMA node of the thread that uses them).
static void init_image(const int nx, const int ny, const int height,
                       real* image, real* tmp_image)
{
  const int tile_size = 64;
#pragma omp parallel for schedule(static)
  for (int i = -1; i <= nx; ++i) {
    real* col = image + (size_t)(i + 1) * height;
    real* tcol = tmp_image + (size_t)(i + 1) * height;
    for (int j = 0; j < height; ++j) {
      col[j] = 0;
      tcol[j] = 0;
    }
    if (i < 0 || i >= nx) continue;
    const int ib = (i / tile_size) * tile_size;
    // checkerboard pattern
    for (int j = 0; j < ny; ++j) {
      const int jb = (j / tile_size) * tile_size;
      if ((ib + jb) % (tile_size * 2)) col[j + 1] = 100;
    }
  }
}

// Routine to output the image in Netpbm grayscale binary image format
static void output_image(const char* file_name, const int nx, const int ny,
                         const int height, const real* image)
{
  // Open output file
  FILE* fp = fopen(file_name, "wb");
  if (!fp) {
    fprintf(stderr, "Error: Could not open %s\n", file_name);
    exit(EXIT_FAILURE);
  }

  // Ouptut image header
  fprintf(fp, "P5 %d %d 255\n", nx, ny);

  // Calculate maximum value of image
  // This is used to rescale the values
  // to a range of 0-255 for output
  double maximum = 0.0;
  for (int i = 0; i < nx; ++i) {
    const real* c = image + (size_t)(i + 1) * height + 1;
    for (int j = 0; j < ny; ++j) {
      if (c[j] > maximum) maximum = c[j];
    }
  }

  // Output image, converting to numbers 0-255 (row-major order)
  unsigned char* line = malloc((size_t)nx);
  for (int j = 0; j < ny; ++j) {
    for (int i = 0; i < nx; ++i) {
      line[i] = (unsigned char)(255.0 * image[(size_t)(i + 1) * height + j + 1] /
                                maximum);
    }
    fwrite(line, 1, (size_t)nx, fp);
  }
  free(line);

  // Close the file
  fclose(fp);
}

// Get the current time in seconds since the Epoch
static double wtime(void)
{
#ifdef _OPENMP
  return omp_get_wtime();
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec * 1e-6;
#endif
}
