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

## Jouer en réseau

Le serveur accueille jusqu'à 32 clients dans un salon. Chaque client reçoit la liste des autres joueurs connectés ; les joueurs en partie y apparaissent avec le statut « in game » et ne peuvent pas être défiés. Les spectateurs apparaissent avec le statut « spectating ». Les joueurs en partie et les spectateurs ne reçoivent plus les mises à jour du salon. La commande `chat <message>` permet aux joueurs présents dans le salon d'échanger des messages ; pendant une partie, elle permet aux deux joueurs de discuter, et les spectateurs reçoivent aussi ces messages. Un spectateur ne peut pas envoyer de chat. La commande `bio` permet d'enregistrer jusqu'à 10 lignes de bio en ASCII imprimable, de 80 caractères maximum chacune. Appuyez sur Entrée pour ajouter une ligne vide, ou tapez `/done` pour terminer avant la dixième ligne ; la saisie s'arrête automatiquement après 10 lignes. La commande `bio <pseudo>` affiche la bio d'un joueur, ou indique qu'il n'en a pas encore enregistré. Pour devenir amis, utilisez `friend add <pseudo>` ; le joueur doit accepter avec `friend accept <pseudo>` ou refuser avec `friend reject <pseudo>`. Consultez les demandes reçues avec `friend requests`, et vos amis avec `friends`. `friend remove <pseudo>` retire l'amitié des deux côtés. Amis et demandes sont conservés en mémoire et supprimés des deux côtés lorsqu'un joueur se déconnecte. `private on` limite l'observation aux amis d'au moins un des deux joueurs ; être ami avec Alice ou Bob suffit. `private off` désactive cette restriction. Les spectateurs déjà présents sont renvoyés au salon si le changement de confidentialité leur retire l'accès. Les parties terminées sont archivées dans `games.txt`, dans le répertoire depuis lequel le serveur a été lancé. Utilisez `games` pour afficher les parties sauvegardées et `review <id>` pour revoir le plateau coup par coup. Seules les parties terminées normalement sont archivées ; une déconnexion en cours de partie n'enregistre pas de replay. Les commandes du salon sont `list`, `bio`, `bio <pseudo>`, `friend add <pseudo>`, `friend accept <pseudo>`, `friend reject <pseudo>`, `friend requests`, `friend remove <pseudo>`, `friends`, `private on`, `private off`, `games`, `review <id>`, `chat <message>`, `challenge <pseudo>`, `accept <pseudo>`, `reject <pseudo>`, `observe <pseudo>` et `quit`. Les pseudos doivent être uniques, sans espace et limités à 31 caractères.

Une fois le défi accepté, les clients vérifient et appliquent localement les règles avec `awale.c`, tandis que le serveur relaie les coups et vérifie le tour et le côté du trou. Il ne vérifie pas toutes les règles : ce prototype est destiné à des joueurs de confiance.

Sous Windows avec MinGW, compilez depuis la racine du projet :

```powershell
gcc -Wall -Wextra -o server.exe server/server.c -lws2_32
gcc -Wall -Wextra -o client.exe client/client.c awale.c -lws2_32
```

Lancez le serveur, avec `0` pour l'horaire ou `1` pour l'antihoraire :

```powershell
.\server.exe 0
```

Puis lancez un client dans chaque terminal :

```powershell
.\client.exe 127.0.0.1 Alice
.\client.exe 127.0.0.1 Bob
```

Pour jouer sur deux machines, remplacez `127.0.0.1` par l'adresse IP de la machine qui héberge le serveur et autorisez le port TCP `1977` dans son pare-feu. Arrêtez le serveur avec Ctrl+C.
