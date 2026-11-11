/* rr_world_marker_select.h - Maintain a bounded set of nearby world markers. */
#ifndef RR_WORLD_MARKER_SELECT_H
#define RR_WORLD_MARKER_SELECT_H
/* Keep at most limit entries for one category. With a valid origin, replace
 * the farthest entry only when a closer candidate arrives. */
static int rr_world_marker_reserve(float* scores, int begin, int* total, int limit,
                                   float score, int have_origin) {
    if (*total - begin < limit) {
        int at = (*total)++;
        scores[at] = score;
        return at;
    }
    if (!have_origin || limit <= 0) return -1;
    int farthest = begin;
    for (int i = begin + 1; i < *total; ++i)
        if (scores[i] > scores[farthest]) farthest = i;
    if (!(score < scores[farthest])) return -1;
    scores[farthest] = score;
    return farthest;
}
#endif
