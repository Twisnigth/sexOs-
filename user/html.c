// =============================================================================
//  user/html.c -- Mini-moteur de rendu HTML : parseur DOM + mise en page.
// -----------------------------------------------------------------------------
//  Étape 1 : tokenizer HTML tolérant -> arbre DOM (éléments + texte + href).
//  Étape 2 : mise en page "box model" simplifiée (blocs/inline, titres en plus
//            gros, gras, paragraphes/marges, listes à puces) -> liste d'items.
//  Étape 3 : les liens (<a href>) sont marqués sur leurs items (clic -> web.c).
//  Pas de CSS complet ni de JavaScript (cf. limites assumées).
// =============================================================================
#include "html.h"

void *memset(void *, int, unsigned long);
void *memcpy(void *, const void *, unsigned long);
unsigned long strlen(const char *);
void utoa(unsigned long, char *);

// --- Limites (tableaux statiques : tout est borné, jamais d'allocation libre) -
#define MAXNODES 6000
#define ARENA    300000
#define MAXITEMS 16000
#define MAXLINKS 1024
#define HREFLEN  256

#define NT_ELEM 1
#define NT_TEXT 2

typedef struct node {
    unsigned char type;
    char  tag[12];
    int   toff, tlen;          // texte (NT_TEXT) : plage dans 'arena'
    int   href;                // index de lien (élément <a>) ou -1
    struct node *child, *sibling, *parent;
} node_t;

static node_t     nodes[MAXNODES];   static int nnodes;
static char       arena[ARENA];      static int alen;
static char       links[MAXLINKS][HREFLEN]; static int nlinks;
static html_item_t items[MAXITEMS];  static int nitems;
static char       g_title[128];

// --- Couleurs (page claire) --------------------------------------------------
#define C_TEXT 0x181c22
#define C_HEAD 0x0e1118
#define C_LINK 0x2d6cdf
#define C_CODE 0x8a2f25

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int streqi(const char *a, const char *b) {
    int i = 0; for (; a[i] && b[i]; i++) if (lc((unsigned char)a[i]) != lc((unsigned char)b[i])) return 0;
    return a[i] == b[i];
}
static int is_ws(int c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }

static node_t *newnode(unsigned char t) {
    if (nnodes >= MAXNODES) return 0;
    node_t *n = &nodes[nnodes++]; memset(n, 0, sizeof *n); n->type = t; n->href = -1; return n;
}
static void addchild(node_t *p, node_t *c) {
    if (!p || !c) return;
    c->parent = p;
    if (!p->child) { p->child = c; return; }
    node_t *s = p->child; while (s->sibling) s = s->sibling; s->sibling = c;
}
// Copie une chaîne dans l'arène (stable) ; renvoie son offset, ou -1.
static int arena_put(const char *s, int len) {
    if (alen + len + 1 > ARENA) return -1;
    int off = alen; memcpy(arena + off, s, len); alen += len; arena[alen++] = 0; return off;
}

// --- Décodage d'entités HTML -------------------------------------------------
static int named_entity(const char *e) {
    if (streqi(e, "lt")) return '<';
    if (streqi(e, "gt")) return '>';
    if (streqi(e, "amp")) return '&';
    if (streqi(e, "quot")) return '"';
    if (streqi(e, "apos")) return '\'';
    if (streqi(e, "nbsp")) return ' ';
    if (streqi(e, "copy")) return 'c';
    if (streqi(e, "reg")) return 'r';
    if (streqi(e, "mdash") || streqi(e, "ndash")) return '-';
    if (streqi(e, "rsquo") || streqi(e, "lsquo")) return '\'';
    if (streqi(e, "rdquo") || streqi(e, "ldquo")) return '"';
    if (streqi(e, "hellip")) return '.';
    return -1;
}
// Ajoute un point de code (best-effort Latin-1) dans le tampon de sortie.
static void emit_cp(char *out, int *o, int omax, int cp) {
    if (*o >= omax) return;
    if (cp >= 32 && cp < 256) out[(*o)++] = (char)cp;
    else if (cp == '\t' || cp == '\n') out[(*o)++] = ' ';
    else out[(*o)++] = (cp > 255) ? '?' : ' ';
}
// Décode le texte src[0..n) (entités + UTF-8 best-effort) dans out ; renvoie len.
static int decode_text(const char *src, int n, char *out, int omax) {
    int o = 0;
    for (int i = 0; i < n && o < omax; ) {
        unsigned char c = (unsigned char)src[i];
        if (c == '&') {
            int j = i + 1, k = 0; char e[12];
            while (j < n && src[j] != ';' && !is_ws(src[j]) && src[j] != '<' && k < 11) e[k++] = src[j++];
            e[k] = 0;
            int adv = (j < n && src[j] == ';');
            int cp = -1;
            if (e[0] == '#') {
                int v = 0;
                if (e[1] == 'x' || e[1] == 'X') { for (int p = 2; e[p]; p++) { int d = e[p]; v = v*16 + ((d>='0'&&d<='9')?d-'0':(lc(d)>='a'&&lc(d)<='f')?lc(d)-'a'+10:0); } }
                else { for (int p = 1; e[p]; p++) if (e[p]>='0'&&e[p]<='9') v = v*10 + (e[p]-'0'); }
                cp = v;
            } else cp = named_entity(e);
            if (cp >= 0) { emit_cp(out, &o, omax, cp); i = j + (adv ? 1 : 0); continue; }
            out[o++] = '&'; i++; continue;       // entité inconnue : '&' littéral
        }
        if (c < 0x80) { out[o++] = (char)c; i++; }
        else if (c >= 0xC2 && c <= 0xDF && i + 1 < n) {      // UTF-8 2 octets
            int cp = ((c & 0x1F) << 6) | ((unsigned char)src[i+1] & 0x3F);
            emit_cp(out, &o, omax, cp); i += 2;
        } else if (c >= 0xE0 && i + 2 < n) { emit_cp(out, &o, omax, '?'); i += 3; } // 3 octets -> ?
        else { i++; }                                        // octet isolé : ignoré
    }
    return o;
}

// --- Éléments vides (auto-fermants) ------------------------------------------
static int is_void(const char *t) {
    return streqi(t,"br")||streqi(t,"hr")||streqi(t,"img")||streqi(t,"meta")||
           streqi(t,"link")||streqi(t,"input")||streqi(t,"area")||streqi(t,"base")||
           streqi(t,"col")||streqi(t,"embed")||streqi(t,"source")||streqi(t,"track")||
           streqi(t,"wbr")||streqi(t,"param");
}

// --- Construction de l'arbre DOM ---------------------------------------------
static node_t *html_build(const char *src, int n) {
    node_t *root = newnode(NT_ELEM); if (root) memcpy(root->tag, "#root", 6);
    node_t *stack[64]; int top = 0; stack[0] = root;
    char tbuf[8192];                                    // tampon de décodage texte

    for (int i = 0; i < n; ) {
        if (src[i] == '<') {
            // Commentaire / doctype / instruction.
            if (i + 3 < n && src[i+1] == '!' && src[i+2] == '-' && src[i+3] == '-') {
                i += 4; while (i + 2 < n && !(src[i]=='-'&&src[i+1]=='-'&&src[i+2]=='>')) i++;
                i = (i + 3 <= n) ? i + 3 : n; continue;
            }
            if (i + 1 < n && (src[i+1] == '!' || src[i+1] == '?')) {
                i += 2; while (i < n && src[i] != '>') i++; if (i < n) i++; continue;
            }
            int closing = 0, j = i + 1;
            if (j < n && src[j] == '/') { closing = 1; j++; }
            // Nom de balise.
            char tag[12]; int tl = 0;
            while (j < n && tl < 11) {
                char c = src[j];
                if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')) { tag[tl++] = (char)lc((unsigned char)c); j++; }
                else break;
            }
            tag[tl] = 0;
            if (tl == 0) { i++; continue; }            // '<' isolé : on saute
            // Reste de la balise (attributs), jusqu'à '>'.
            int attr_start = j;
            while (j < n && src[j] != '>') j++;
            int attr_end = j; if (j < n) j++;          // saute '>'
            int self_close = (attr_end > attr_start && src[attr_end-1] == '/');

            if (closing) {
                // Ferme jusqu'à la balise correspondante (récupération d'erreurs).
                int k = top;
                while (k > 0 && !streqi(stack[k]->tag, tag)) k--;
                if (k > 0) top = k - 1;
                i = j; continue;
            }
            // script / style : on saute le contenu brut jusqu'à la fermeture.
            if (streqi(tag, "script") || streqi(tag, "style")) {
                while (j < n) {
                    if (src[j] == '<' && j+1 < n && src[j+1] == '/') {
                        int m = j + 2, q = 0;
                        while (m < n && tag[q] && lc((unsigned char)src[m]) == tag[q]) { m++; q++; }
                        if (!tag[q]) { while (m < n && src[m] != '>') m++; if (m < n) m++; j = m; break; }
                    }
                    j++;
                }
                i = j; continue;
            }
            node_t *e = newnode(NT_ELEM);
            if (!e) { i = j; continue; }
            memcpy(e->tag, tag, tl + 1);
            // href des liens.
            if (streqi(tag, "a") && nlinks < MAXLINKS) {
                const char *a = src + attr_start; int al = attr_end - attr_start;
                for (int p = 0; p + 5 < al; p++) {
                    if (lc((unsigned char)a[p])=='h'&&lc((unsigned char)a[p+1])=='r'&&
                        lc((unsigned char)a[p+2])=='e'&&lc((unsigned char)a[p+3])=='f'&&a[p+4]=='=') {
                        int q = p + 5; char quote = 0;
                        if (q < al && (a[q]=='"'||a[q]=='\'')) quote = a[q++];
                        int h = 0;
                        while (q < al && h < HREFLEN-1 && (quote ? a[q]!=quote : !is_ws(a[q]) && a[q]!='>')) links[nlinks][h++] = a[q++];
                        links[nlinks][h] = 0; e->href = nlinks++; break;
                    }
                }
            }
            addchild(stack[top], e);
            if (!is_void(tag) && !self_close && top < 63) stack[++top] = e;
            i = j; continue;
        }
        // Texte jusqu'au prochain '<'.
        int ts = i; while (i < n && src[i] != '<') i++;
        int rawlen = i - ts;
        if (rawlen > 0) {
            int dl = decode_text(src + ts, rawlen, tbuf, sizeof tbuf);
            // Ignore le texte purement blanc (entre balises de bloc).
            int allws = 1; for (int k = 0; k < dl; k++) if (!is_ws(tbuf[k])) { allws = 0; break; }
            if (!allws) {
                int off = arena_put(tbuf, dl);
                if (off >= 0) { node_t *t = newnode(NT_TEXT); if (t) { t->toff = off; t->tlen = dl; addchild(stack[top], t); } }
            }
        }
    }
    return root;
}

// --- Récupère le <title> -----------------------------------------------------
static void find_title(node_t *n) {
    if (!n || g_title[0]) return;
    if (n->type == NT_ELEM && streqi(n->tag, "title") && n->child && n->child->type == NT_TEXT) {
        int l = n->child->tlen; if (l > 127) l = 127;
        memcpy(g_title, arena + n->child->toff, l); g_title[l] = 0; return;
    }
    for (node_t *c = n->child; c; c = c->sibling) find_title(c);
}

// --- Mise en page ------------------------------------------------------------
typedef struct { short scale, bold, under, link; uint32_t color; } style_t;

static int pen_x, pen_y, line_left, line_right, line_h, line_has, pend_sp;
static struct { short ordered, count; } lstk[32]; static int ltop;

static void line_break(void) {           // saut de ligne (au moins une hauteur)
    pen_y += (line_h > 0 ? line_h : 16);
    pen_x = line_left; line_h = 0; line_has = 0; pend_sp = 0;
}
static void flush_line(void) {           // termine la ligne si elle a du contenu
    if (line_has) { pen_y += line_h; line_h = 0; line_has = 0; }
    pen_x = line_left; pend_sp = 0;
}
static void add_word(const char *t, int len, style_t s) {
    if (len <= 0) return;
    int cw = 8 * s.scale, chh = 16 * s.scale, w = len * cw;
    int sp = (pend_sp && line_has) ? cw : 0;
    if (line_has && pen_x + sp + w > line_right && w <= (line_right - line_left)) { line_break(); sp = 0; }
    pen_x += sp;
    if (nitems < MAXITEMS) {
        html_item_t *it = &items[nitems++];
        it->x = pen_x; it->y = pen_y; it->w = w; it->h = chh;
        it->scale = s.scale; it->bold = s.bold; it->under = s.under;
        it->link = s.link; it->color = s.color; it->text = t; it->len = len;
    }
    pen_x += w; if (chh > line_h) line_h = chh; line_has = 1; pend_sp = 0;
}
static void layout_text(node_t *n, style_t s) {
    const char *t = arena + n->toff; int len = n->tlen, i = 0;
    while (i < len) {
        int sawsp = 0;
        while (i < len && is_ws(t[i])) { i++; sawsp = 1; }
        if (sawsp) pend_sp = 1;
        if (i >= len) break;
        int ws = i; while (i < len && !is_ws(t[i])) i++;
        add_word(t + ws, i - ws, s);
    }
}

static void layout_node(node_t *n, style_t s) {
    if (!n) return;
    if (n->type == NT_TEXT) { layout_text(n, s); return; }
    const char *tag = n->tag;
    // Éléments non rendus.
    if (streqi(tag,"head")||streqi(tag,"script")||streqi(tag,"style")||streqi(tag,"title")||
        streqi(tag,"meta")||streqi(tag,"link")||streqi(tag,"noscript")) return;

    style_t cs = s; int block = 0, mtop = 0, mbot = 0;
    int is_list = 0, is_li = 0;

    if      (streqi(tag,"h1")) { block=1; cs.scale=3; cs.bold=1; cs.color=C_HEAD; mtop=14; mbot=8; }
    else if (streqi(tag,"h2")) { block=1; cs.scale=2; cs.bold=1; cs.color=C_HEAD; mtop=12; mbot=6; }
    else if (streqi(tag,"h3")) { block=1; cs.scale=2; cs.bold=1; cs.color=C_HEAD; mtop=8;  mbot=4; }
    else if (streqi(tag,"h4")||streqi(tag,"h5")||streqi(tag,"h6")) { block=1; cs.bold=1; cs.color=C_HEAD; mtop=6; mbot=4; }
    else if (streqi(tag,"p"))  { block=1; mtop=6; mbot=6; }
    else if (streqi(tag,"br")) { line_break(); return; }
    else if (streqi(tag,"hr")) { flush_line(); pen_y += 10; return; }
    else if (streqi(tag,"ul")||streqi(tag,"ol")) { block=1; is_list=1; mtop=4; mbot=4; }
    else if (streqi(tag,"li")) { block=1; is_li=1; }
    else if (streqi(tag,"blockquote")) { block=1; mtop=4; mbot=4; }
    else if (streqi(tag,"pre")) { block=1; mtop=4; mbot=4; }
    else if (streqi(tag,"tr"))  { block=1; }
    else if (streqi(tag,"div")||streqi(tag,"section")||streqi(tag,"article")||streqi(tag,"header")||
             streqi(tag,"footer")||streqi(tag,"main")||streqi(tag,"nav")||streqi(tag,"figure")||
             streqi(tag,"table")||streqi(tag,"form")||streqi(tag,"figcaption")||streqi(tag,"aside")) { block=1; mtop=2; mbot=2; }
    else if (streqi(tag,"a"))  { if (n->href >= 0) { cs.link = (short)n->href; cs.color = C_LINK; cs.under = 1; } }
    else if (streqi(tag,"b")||streqi(tag,"strong")) { cs.bold = 1; }
    else if (streqi(tag,"u")||streqi(tag,"ins")) { cs.under = 1; }
    else if (streqi(tag,"code")||streqi(tag,"tt")||streqi(tag,"kbd")||streqi(tag,"samp")) { cs.color = C_CODE; }

    int saved_left = line_left;
    if (block) { flush_line(); pen_y += mtop; }
    if (is_list) { line_left += 22; pen_x = line_left; if (ltop < 32) { lstk[ltop].ordered = streqi(tag,"ol"); lstk[ltop].count = 0; ltop++; } }
    if (is_li) {
        flush_line();
        char b[12]; int bl;
        if (ltop > 0 && lstk[ltop-1].ordered) {        // liste ordonnée : "N."
            lstk[ltop-1].count++;
            char num[10]; utoa((unsigned)lstk[ltop-1].count, num);
            bl = 0; for (char *q = num; *q; q++) b[bl++] = *q; b[bl++] = '.'; b[bl] = 0;
        } else {                                       // liste à puces : "-"
            if (ltop > 0) lstk[ltop-1].count++;
            b[0] = '-'; b[1] = 0; bl = 1;
        }
        int off = arena_put(b, bl);
        if (off >= 0) add_word(arena + off, bl, s);
        pend_sp = 1;
    }

    for (node_t *c = n->child; c; c = c->sibling) layout_node(c, cs);

    if (is_list) { if (ltop > 0) ltop--; line_left = saved_left; pen_x = line_left; }
    if (block)   { flush_line(); pen_y += mbot; line_left = saved_left; }
}

// --- API ---------------------------------------------------------------------
int html_render(const char *src, int n, int content_w, int *total_h) {
    nnodes = 0; alen = 0; nlinks = 0; nitems = 0; g_title[0] = 0; ltop = 0;
    node_t *root = html_build(src, n);
    find_title(root);
    pen_x = line_left = 0; line_right = content_w; pen_y = 0; line_h = 0; line_has = 0; pend_sp = 0;
    style_t def = { 1, 0, 0, -1, C_TEXT };
    if (root) for (node_t *c = root->child; c; c = c->sibling) layout_node(c, def);
    flush_line();
    if (total_h) *total_h = pen_y + 8;
    return nitems;
}
const html_item_t *html_items(void) { return items; }
const char *html_title(void) { return g_title; }
const char *html_link_at(int dx, int dy) {
    for (int i = 0; i < nitems; i++) {
        html_item_t *it = &items[i];
        if (it->link < 0) continue;
        if (dx >= it->x && dx < it->x + it->w && dy >= it->y && dy < it->y + it->h)
            return links[it->link];
    }
    return 0;
}
