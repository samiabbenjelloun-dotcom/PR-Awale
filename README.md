# PR-Awale

Jeu d'Awale en C, jouable à deux joueurs dans le terminal.

## Prérequis

Il faut un compilateur C, par exemple GCC.

## Compiler et lancer

Depuis le dossier du projet, compilez les deux fichiers source :

```bash
gcc -Wall -Wextra -o awale main.c awale.c
```

Puis lancez le programme :

```bash
./awale
```

Sous Windows avec MinGW, la compilation crée `awale.exe`. Lancez-le avec :

```powershell
.\awale.exe
```

Au démarrage, choisissez le sens du joueur 0 : `0` pour clockwise ou `1` pour anticlockwise. Le joueur 1 joue dans le sens opposé. Entrez ensuite le numéro du trou à jouer lorsque le programme le demande.

## Fichiers principaux

- `main.c` : boucle de jeu et saisie des coups.
- `awale.c` : règles et logique du jeu.
- `awale.h` : structures et déclarations partagées.
