# ClokeOS: Schnelle Aufzählung liefert nur lebende Einträge und genau so viele, wie sie in den Puffer schreibt. GSIMaps Fassung
# zählt vorab nodeCount und lässt Lücken (Müll im Puffer), wenn schwache Einträge unterwegs wegfallen; Logic Pros
# MAUISyncManager stürzt dabei in objc_msgSend ab.
/^- \(NSUInteger\) countByEnumeratingWithState: \(NSFastEnumerationState\*\)state[[:space:]]*$/,/^\}$/{
/^  return GSIMapCountByEnumeratingWithStateObjectsCount$/c\
  {\
    struct clk_part { GSIMapNode node; uintptr_t bucket; } *part = (struct clk_part *)state->extra;\
    GSIMapEnumerator_t e;\
    NSUInteger n = 0;\
    if (state->state == 0)\
      e = GSIMapEnumeratorForMap(self);\
    else\
      {\
        if (part->node == 0)\
          return 0;\
        e.map = self;\
        e.node = part->node;\
        e.bucket = part->bucket;\
      }\
    while (n < len)\
      {\
        GSIMapNode node = GSIMapEnumeratorNextNode(&e);\
        if (node == 0)\
          break;\
        id o = (id)GSI_MAP_READ_KEY(self, &node->key).addr;\
        if (o != nil)\
          stackbuf[n++] = o;\
      }\
    part->node = e.node;\
    part->bucket = e.bucket;\
    state->state = 1;\
    state->itemsPtr = stackbuf;\
    return n;\
  }
/^    \(self, state, stackbuf, len\);$/d
}
