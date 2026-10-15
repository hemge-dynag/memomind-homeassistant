# MemoMind Home Assistant

App **pairée** MemoMind (lunettes + téléphone) qui affiche un **tableau de bord
Home Assistant** directement sur les lunettes. On change de page en tournant la
tête à **gauche** ou à **droite**, on déplace une **sélection** sur les lignes en
levant/baissant la tête, et on **exécute l'action** de la ligne sélectionnée
(allumer/éteindre une lampe, un interrupteur, lancer une scène…) en appuyant sur
le **bouton** — le tout sans les mains. Côté téléphone, une **liste d'entités
avec recherche** permet de composer plusieurs **pages** à la carte.

## Architecture

```
Home Assistant (REST API /api)
        ^
        |  HTTP(S) + Bearer <Long-Lived Token>   (reste sur le téléphone)
        |
   Téléphone : gm-plugin-web (web plugin)
        |  lit /api/states, mappe les entités en pages
        |  gm.plugin.sendMessage(channel 0x4841 = 'HA')
        v
   Lunettes : plugin GMP paire
        |  affiche titre + "i/n" + la fenetre visible de lignes label/valeur
        |  gestes tete LEFT/RIGHT = page precedente/suivante
        |          tete HEAD_RAISE/HEAD_LOWER = deplacer la selection
        |          bouton PRIMARY = executer l'action de la ligne selectionnee
        |  gm ... bt_send(channel 0x4853 = 'HS', EVENT_PAGE_CHANGED / EVENT_ACTION)
        v
   Telephone : agit (POST /api/services/<domain>/<service>) puis resynchronise
```

- Le **jeton longue duree** Home Assistant ne quitte jamais le telephone : il
  n'est jamais envoye aux lunettes, il reste dans le stockage prive du plugin et
  dans l'en-tete `Authorization`.
- Les lunettes ne parlent **jamais** a Home Assistant directement ; tout le
  reseau reste cote telephone. Le bouton des lunettes ne fait qu'envoyer
  l'identifiant de l'entite selectionnee au telephone, qui appelle le service.

## Configuration (cote telephone)

1. **URL de Home Assistant** — ex. `http://homeassistant.local:8123`
   (ou une URL HTTPS si vous passez par un reverse proxy / Nabu Casa).
2. **Jeton d'acces longue duree** — cree dans Home Assistant :
   *profil -> Securite -> Jetons d'acces longue duree*.
3. **Charger les entites** — recupere toutes les entites de votre instance et
   les affiche dans une liste **filtrable par recherche**. Cochez celles a
   afficher : elles sont ajoutees a la **page active**.
4. **Pages** — **Ajouter une page** pour creer d'autres pages, donnez-leur un
   titre et repartissez vos entites. Chaque page est independante.
5. **Actualisation automatique** (toutes les 60 s) en option.

Boutons : **Enregistrer**, **Tester la connexion**, **Charger les entites**,
**Synchroniser**.

## Navigation sur les lunettes

- **Tete a droite** : page suivante.
- **Tete a gauche** : page precedente.
- **Tete levee** : selection vers le haut.
- **Tete baissee** : selection vers le bas.
- **Bouton** : execute l'action de la ligne selectionnee (l'unite est
  allumee/eteinte, la scene est lancee…).
- Navigation **circulaire** (on revient a la premiere apres la derniere).

Chaque page affiche un titre avec sa position `i/n` (et `lignes visibles/total`
quand la page defile) et les lignes `libelle : valeur` qui tiennent a l'ecran.
La ligne selectionnee est mise en surbrillance et reste toujours visible (la
page defile automatiquement pour la suivre). Aucun texte de rappel n'est affiche
en bas d'ecran : les gestes se suffisent a eux-memes et l'espace est reserve aux
lignes.

## Actions (bouton)

Le bouton des lunettes envoie l'`entity_id` de la ligne selectionnee au
telephone, qui appelle le service Home Assistant correspondant au domaine :

- `light`, `switch`, `fan`, `cover`, … -> `toggle`
- `scene`, `script`, `automation` -> `turn_on`
- `button` -> `press`

Les entites sans action utile (capteurs, etc.) resteront sans effet. Les valeurs
se rafraichissent automatiquement apres chaque action.

## Limites

- Les valeurs sont **textuelles** uniquement (etat + unite de mesure). Pas de
  graphiques ni de cartes Home Assistant sur les lunettes.
- Jusqu'a **32 lignes** par page (seules les lignes qui tiennent a l'ecran sont
  dessinees ; les autres sont atteintes en defilant), **24 pages** maximum
  (contraintes de memoire du plugin lunettes : RAM statique < 100 KiB,
  Flash <= 500 KiB).
- Libelle d'entite tronque a **32 octets**, valeur a **24 octets**, titre de
  page a **32 octets** (decoupage UTF-8 propre).
- Le nom convivial (`friendly_name`) de l'entite est utilise comme libelle s'il
  existe, sinon l'`entity_id` sans son domaine.
- Si aucune entite ne se resout, les lunettes reviennent a l'ecran d'attente.

## Identifiants

- Lunettes : `com.dynag.homeassistant.glass` (protocole `gm.homeassistant` 1.0)
- Telephone : `com.dynag.homeassistant.phone`
- Canaux de messages : `0x4841` ('HA', telephone -> lunettes),
  `0x4853` ('HS', lunettes -> telephone).
- Evenements lunettes -> telephone : `EVENT_PAGE_CHANGED`, `EVENT_ACTION`
  (action de la ligne selectionnee).

## Structure du depot

```
glass/homeassistant/    # homeassistant.c + manifest.json (GMP lunettes)
phone/homeassistant/    # index.html, plugin.js, protocol.js, ha.js,
                        # translations.js, style.css, vendor/, manifest.json
builds/                 # homeassistant.gmp, homeassistant-1.2.0.mmpkg + zips sources
```

## Compilation

Depuis la racine du SDK Plugin Open Platform :

```bash
export PATH="$HOME/.local/bin:$PATH"
python3 build.py glass   # -> GlassSDK/build-host/.build/homeassistant/homeassistant.gmp
python3 build.py web     # -> PhoneSDK/dist/homeassistant-1.2.0.mmpkg
```

Le SDK decouvre automatiquement l'exemple via les `manifest.json` sous
`GlassSDK/examples` et `PhoneSDK/examples`.

## Langues

Interface telephone traduite en 6 langues : francais (defaut), anglais,
espagnol, italien, allemand, chinois simplifie. Le texte affiche sur les
lunettes s'adapte aussi a la langue systeme de l'appareil.
