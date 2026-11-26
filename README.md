# Stencil 5 points : version optimisée et parallélisée

Ce dépôt contient une version optimisée du stencil 5 points pondéré
(`stencil.c`). Le code d'origine est conservé dans `original/` pour comparaison.

Chaque cellule est remplacée par `0,6 × centre + 0,1 × (N + S + E + O)`, les
cellules hors de la grille valant 0. La grille est initialisée en damier de
64×64. On fait 2 balayages par itération et le résultat est écrit dans
`stencil.pgm`. Seule la boucle de calcul est chronométrée.

## Résultats (machine de développement, 4 cœurs Xeon à 2,1 GHz, AVX-512)

| Problème        | Original (`gcc -std=c99`, 1 cœur) | Optimisé (4 threads) | Accélération |
| --------------- | --------------------------------: | -------------------: | -----------: |
| 1024 × 1024 × 100 |                            8,89 s |             ≈ 0,015 s |       ≈ 600× |
| 4096 × 4096 × 100 |                          179,3 s |              ≈ 0,30 s |       ≈ 590× |
| 8000 × 8000 × 100 |                          876,5 s |              ≈ 1,05 s |       ≈ 830× |

Les trois sorties passent `check.py` (écart maximal de 1 niveau de gris, la
tolérance autorisée étant ±1). Les temps sont des médianes de quelques
exécutions sur une VM partagée, donc à ±15 % près.

### Gain de chaque optimisation (8000 × 8000 × 100)

| Étape                                                          | Temps   |
| -------------------------------------------------------------- | ------: |
| Code d'origine, sans optimisation du compilateur               | 876,5 s |
| Boucles permutées, halo, `-O3 -march=native`, double, 1 cœur   |  20,3 s |
| + simple précision (`float`)                                   |  10,3 s |
| + blocage temporel (1 cœur)                                    |   4,2 s |
| + OpenMP 4 threads, sans blocage temporel                      |   3,1 s |
| + OpenMP 4 threads et blocage temporel (T = 32)                | ≈ 1,05 s |

Pour comparaison, le code d'origine compilé en `-O3` met 77,5 s sur
4096 × 4096 au lieu de 179 s. Le compilateur seul ne corrige donc pas le
problème principal, qui est l'ordre des boucles.

## Résultats sur ROMEO (nœud x64cpu : 2 × AMD EPYC 9654, 192 cœurs, 8 nœuds NUMA)

Compilé avec gcc 14.2.0 (`-O3 -march=native -mprefer-vector-width=512 -fopenmp`),
avec `OMP_PROC_BIND=close` et `OMP_PLACES=cores`. Ces résultats viennent de
`compare.job` : le code d'origine est compilé comme dans le Makefile initial
(`gcc -std=c99`) et tourne sur 1 cœur, et l'on garde le meilleur temps de 5
exécutions de la version optimisée.

### Comparatif : version d'origine vs version optimisée

| Problème          | Original, 1 cœur | Optimisé, 192 threads | Accélération | check.py |
| ----------------- | ---------------: | --------------------: | -----------: | -------- |
| 1024 × 1024 × 100 |          3,858 s |              0,0073 s |       × 527  | OK (écart max 1) |
| 4096 × 4096 × 100 |         80,152 s |              0,0259 s |      × 3 100 | OK (écart max 1) |
| 8000 × 8000 × 100 |        186,685 s |              0,0321 s |      × 5 819 | OK (écart max 1) |

Réglages choisis automatiquement : découpage 12 × 16 blocs, T = 32 (T = 8 pour
1024), BJ = 1343.

Le code d'origine est anormalement lent sur 4096 : 21 fois le temps de 1024
pour 16 fois plus de points. C'est probablement dû au pas d'accès de
4096 × 8 octets, une puissance de 2 qui fait tomber les accès sur les mêmes
ensembles du cache. La version optimisée n'a pas ce problème, car elle lit la
mémoire de façon contiguë.

### Passage à l'échelle sur 8000 × 8000 × 100 (`scaling.job`, en secondes)

| threads (découpage) | T auto | T=16  | T=32  |
| ------------------: | -----: | ----: | ----: |
| 1 (1×1)             | 2,261  | 2,242 | 2,260 |
| 2 (1×2)             | 1,135  | 1,155 | 1,138 |
| 4 (2×2)             | 0,579  | 0,587 | 0,576 |
| 8 (2×4)             | 0,309  | 0,302 | 0,309 |
| 16 (4×4)            | 0,162  | 0,160 | 0,161 |
| 32 (4×8)            | 0,085  | 0,108 | 0,085 |
| 64 (8×8)            | 0,047  | 0,059 | 0,047 |
| 96 (8×12)           | 0,041  | 0,066 | 0,041 |
| 128 (8×16)          | 0,033  | 0,057 | 0,052 |
| 192 (12×16)         | 0,029 à 0,050 | 0,053 | 0,049 |

L'accélération est quasi linéaire jusqu'à 64 threads (× 48), et le temps
continue de baisser jusqu'à 128 threads. À 192 threads, un calcul complet ne
dure plus que 30 à 50 ms. On est alors limité par la bande passante mémoire et
les barrières, et le temps varie d'une exécution à l'autre.

Le découpage 2D (blocs de colonnes × blocs de lignes) permet de garder T = 32
même avec 192 threads. Avec l'ancien découpage en colonnes seulement, il
fallait descendre à T = 5 : sur 8000², le temps est passé de 0,040 s avec le
découpage 1D à environ 0,030 s.

## Optimisations appliquées

1. **Accès mémoire contigus.** Le tableau est stocké `image[j + i*height]`,
   mais la boucle interne de l'original parcourait `i`, donc avec un pas de
   `height` doubles, ce qui produit un défaut de cache à chaque accès. La boucle
   interne parcourt maintenant `j`.
2. **Halo de zéros.** La grille est allouée avec une colonne et une ligne de
   zéros de chaque côté. Les 4 `if` de bord disparaissent et la boucle interne
   est entièrement vectorisée (`#pragma omp simd`, AVX-512 sur x86).
3. **Simple précision.** Passer en `float` divise par deux le volume de
   données et double la largeur SIMD. Le résultat reste dans la tolérance ±1.
   Pour garder la double précision, compiler avec `make double`.
4. **Micro-optimisations.** Les constantes 0,6 et 0,1 sont précalculées, les
   pointeurs sont déclarés `restrict` et les allocations sont alignées sur
   64 octets.
5. **Blocage temporel (wavefront).** Un stencil est limité par la bande
   passante mémoire : il fait 6 opérations pour 8 octets déplacés. On fusionne
   donc T balayages en un seul passage sur la grille. Les T−1 niveaux
   intermédiaires ne sont gardés que sur 3 colonnes chacun (un anneau qui
   reste en cache). La grille ne passe par la mémoire qu'une fois tous les T
   balayages au lieu d'une fois par balayage.
6. **Blocage spatial en j.** Les colonnes sont découpées en bandes de BJ lignes,
   avec un halo de T lignes recalculé, pour que l'anneau tienne dans la moitié
   du cache L2. BJ est calculé automatiquement à partir de la taille du L2.
7. **OpenMP, découpage 2D.** La grille est découpée en px × py blocs
   (colonnes × lignes, les plus carrés possible), un par thread. Les T
   colonnes ou lignes de chaque bord sont recalculées de façon redondante, si bien qu'aucune
   synchronisation n'est nécessaire pendant un passage : il n'y a qu'une
   barrière tous les T balayages. L'initialisation est elle aussi parallèle
   (« first touch »), pour que les pages mémoire soient placées sur le nœud
   NUMA du thread qui les utilise, ce qui compte sur les nœuds bi-socket de
   ROMEO.

## Compiler et tester sur un PC personnel

Prérequis : `gcc` avec OpenMP (ou `clang` avec `libomp`), `make`, `python3`.

```bash
make                 # binaire ./stencil (OpenMP, float, -O3 -march=native)
./run_local.sh       # lance 1024 et 4096 puis vérifie avec check.py
./run_local.sh 8000  # une taille précise
make check           # 1024x1024x100 + vérification
make bench           # compare original et optimisé sur 1024x1024x100
```

Autres cibles : `make double` (double précision), `make serial` (sans OpenMP),
`make original` (code d'origine).

- **Linux / WSL :** tout fonctionne directement avec `gcc`.
- **macOS :** le `clang` d'Apple ne gère pas OpenMP. Installer gcc avec
  `brew install gcc` puis lancer `make CC=gcc-14` (adapter le numéro de
  version).

## Benchmark sur ROMEO

```bash
sbatch stencil.job    # 3 tailles + vérification + original 1024 -> stencil.out
sbatch compare.job    # comparatif original vs optimisé, 3 tailles -> compare.out
sbatch scaling.job    # passage à l'échelle 1, 2, 4, ... threads -> scaling.out
```

Les scripts compilent sur le nœud de calcul, pour que `-march=native` cible le
bon processeur. Ils utilisent tous les cœurs du nœud avec
`OMP_PROC_BIND=close` et `OMP_PLACES=cores`. Ils sont réglés pour ROMEO 2025 :
partition `instant`, contrainte `x64cpu`, `--mem 64G` (obligatoire), et gcc
14.2.0 chargé via spack. Le compte `r250127` est à changer si besoin. Les
scripts font tous `make clean` dans le même dossier : ne les lancez pas en
même temps, par exemple `sbatch --dependency=afterany:<id> scaling.job`.

## Réglages (variables d'environnement)

| Variable          | Rôle                                                    | Défaut              |
| ----------------- | ------------------------------------------------------- | ------------------- |
| `OMP_NUM_THREADS` | nombre de threads                                       | tous les cœurs      |
| `STENCIL_T`       | balayages fusionnés par passage (1 = pas de blocage)    | 32 (réduit si peu de colonnes par thread) |
| `STENCIL_BJ`      | hauteur des bandes en j                                 | calculée d'après le L2 |
| `STENCIL_L2`      | taille du L2 en octets, si la détection échoue          | `sysconf`, sinon 512 Kio |
| `STENCIL_PX`      | nombre de blocs de colonnes (doit diviser le nombre de threads) | automatique |

Le programme affiche sur stderr la configuration utilisée, par exemple
`threads=4 (2x2) T=32 BJ=2752 precision=float`.

## Vérifier les résultats

```bash
python3 check.py --ref-stencil-file stencil_1024_1024_100.pgm --stencil-file stencil.pgm
```

`check.py` a été porté en Python 3 (l'original ne tournait qu'en Python 2). Il
renvoie un code de sortie non nul en cas d'échec.

| nx   | ny   | niters | Image de référence          |
| ---- | ---- | ------ | --------------------------- |
| 1024 | 1024 | 100    | `stencil_1024_1024_100.pgm` |
| 4096 | 4096 | 100    | `stencil_4096_4096_100.pgm` |
| 8000 | 8000 | 100    | `stencil_8000_8000_100.pgm` |
