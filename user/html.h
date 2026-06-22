// =============================================================================
//  user/html.h -- Mini-moteur de rendu HTML (parseur DOM + mise en page).
// -----------------------------------------------------------------------------
//  html_render() analyse le HTML en un arbre DOM puis calcule une mise en page
//  "box model" simplifiée : il en sort une liste d'ITEMS (mots positionnés avec
//  taille/gras/couleur/soulignement et éventuel lien). Le navigateur (web.c) se
//  contente de PEINDRE ces items (avec défilement) et de tester les clics.
// =============================================================================
#ifndef SEXOS_HTML_H
#define SEXOS_HTML_H

#include <stdint.h>

// Un mot mis en page, en coordonnées DOCUMENT (origine en haut à gauche du contenu).
typedef struct {
    int      x, y, w, h;      // position et taille (h = hauteur de la ligne du mot)
    short    scale;           // échelle de police (1, 2, 3…)
    short    bold;            // 1 = gras (double tracé)
    short    under;           // 1 = souligné (liens)
    short    link;            // index de lien (>=0) ou -1
    uint32_t color;           // couleur du texte
    const char *text;         // pointeur (stable) sur les caractères du mot
    int      len;             // longueur du mot
} html_item_t;

// Analyse `src` (longueur n) et met en page sur une largeur `content_w` (pixels).
// Renvoie le nombre d'items ; *total_h reçoit la hauteur totale du document (px).
int  html_render(const char *src, int n, int content_w, int *total_h);

// Accès aux items mis en page (valides jusqu'au prochain html_render).
const html_item_t *html_items(void);

// href BRUT du lien situé sous le point document (x,y), ou NULL si aucun lien.
const char *html_link_at(int doc_x, int doc_y);

// Titre de la page (<title>) ou chaîne vide.
const char *html_title(void);

#endif
