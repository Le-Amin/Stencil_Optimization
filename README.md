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
192 threads OpenMP (`OMP_PROC_BIND=close`, `OMP_PLACES=cores`), T choisi
automatiquement.

| Problème          | T | Temps optimisé | check.py |
| ----------------- | -: | -------------: | -------- |
| 1024 × 1024 × 100 | 1 | 0,024 s | OK (écart max 1) |
| 4096 × 4096 × 100 | 2 | 0,024 s | OK (écart max 1) |
| 8000 × 8000 × 100 | 5 | 0,040 s | OK (écart max 1) |

Le code d'origine met 3,87 s sur 1024 × 1024 × 100 (1 cœur), soit un gain
d'environ ×163 sur cette taille. Sur 1024², il y a trop peu de travail pour
192 threads : le temps est dominé par les barrières et le lancement des
threads.

Passage à l'échelle sur 8000 × 8000 × 100 (`scaling.job`, temps en secondes) :

| threads | T=16  | T=32  | T=64  |
| ------: | ----: | ----: | ----: |
| 1       | 2,241 | 2,296 | 2,861 |
| 2       | 1,142 | 1,151 | 1,420 |
| 4       | 0,584 | 0,590 | 0,736 |
| 8       | 0,305 | 0,305 | 0,388 |
| 16      | 0,165 | 0,162 | 0,208 |
| 32      | 0,096 | 0,090 | 0,116 |
| 64      | 0,054 | 0,054 | 0,073 |
| 128     | 0,064 | 0,063 | 0,078 |
| 192     | 0,060 | 0,061 | 0,082 |

L'accélération est presque linéaire jusqu'à 64 threads (×41). Au-delà, avec
T forcé à 16 ou plus, les blocs de colonnes deviennent trop étroits : environ
40 colonnes par thread à 192 threads, donc le calcul redondant des bords
l'emporte. Avec le réglage automatique (T=5 à 192 threads), on obtient le
meilleur temps : 0,040 s.

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
7. **OpenMP.** Chaque thread traite un bloc contigu de colonnes. Les T colonnes
   de chaque bord sont recalculées de façon redondante, si bien qu'aucune
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
sbatch scaling.job    # passage à l'échelle 1, 2, 4, ... threads -> scaling.out
```

Les scripts compilent sur le nœud de calcul, pour que `-march=native` cible le
bon processeur. Ils utilisent tous les cœurs du nœud avec
`OMP_PROC_BIND=close` et `OMP_PLACES=cores`. Ils sont réglés pour ROMEO 2025 :
partition `instant`, contrainte `x64cpu`, `--mem 64G` (obligatoire), et gcc
14.2.0 chargé via spack. Le compte `r250127` est à changer si besoin. Les deux
scripts font `make clean` dans le même dossier : lancez `scaling.job` après
`stencil.job`, par exemple avec `sbatch --dependency=afterany:<id> scaling.job`.

## Réglages (variables d'environnement)

| Variable          | Rôle                                                    | Défaut              |
| ----------------- | ------------------------------------------------------- | ------------------- |
| `OMP_NUM_THREADS` | nombre de threads                                       | tous les cœurs      |
| `STENCIL_T`       | balayages fusionnés par passage (1 = pas de blocage)    | 32 (réduit si peu de colonnes par thread) |
| `STENCIL_BJ`      | hauteur des bandes en j                                 | calculée d'après le L2 |
| `STENCIL_L2`      | taille du L2 en octets, si la détection échoue          | `sysconf`, sinon 512 Kio |

Le programme affiche sur stderr la configuration utilisée, par exemple
`threads=4 T=32 BJ=2752 precision=float`.

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
