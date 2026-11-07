# ClokeOS: dict[key] = nil entfernt wie bei Apple den Schlüssel (dokumentiertes Verhalten von setObject:forKeyedSubscript:), statt wie
# setObject:forKey: eine Ausnahme zu werfen. Logic Pro verlässt sich darauf (Hunderte Ausnahmen, beim Zeichnen tödlich).
/^- \(void\)setObject:\(id\)obj forKeyedSubscript:\(id <NSCopying>\)key$/,/^\}$/s/^    \[self setObject:obj forKey:key\];$/    if (obj == nil)\n        [self removeObjectForKey:key];\n    else\n        [self setObject:obj forKey:key];/
