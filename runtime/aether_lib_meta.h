/* aether_lib_meta.h — symbol-catalog metadata for `--emit=lib`
 * artifacts (issue #403, MVP).
 *
 * Aether compiles to C, which inherits a flat-symbol-table linker
 * model — once a `.ae` is compiled to a `.so`/`.dylib`/`.dll`, the
 * Aether-side namespace, parameter-naming, and source-location
 * info that the source carried is gone from the consumer's
 * perspective. This header defines a small in-binary surface that
 * preserves it: every `--emit=lib` artifact exports a
 * `aether_lib_meta()` function returning a pointer to a static
 * `AetherLibMeta` struct describing the artifact's exports.
 *
 * The format is plain C structs of `const char*` and `int`. No
 * dynamic allocation, no parsing. Any FFI consumer (Python ctypes,
 * Java Panama, Ruby Fiddle, Node-API, hand-rolled `dlsym`) can
 * walk the struct directly. The CLI tool `ae lib-info <path>`
 * dlopens an artifact, calls `aether_lib_meta()`, and prints the
 * catalog in human-readable form.
 *
 * This is the v1 / MVP scope. v2 adds closure-context records
 * (captures + capture types per closure reachable from an export);
 * the schema here reserves a `closure_count` slot already so the
 * struct can grow without ABI break.
 *
 * Schema versioning: `schema_version` is set to "1.0" by every
 * artifact that uses this header. Hosts that read the metadata
 * should refuse anything not "1.<minor>" — within "1.x" the
 * additive fields below are guaranteed.
 */

#ifndef AETHER_LIB_META_H
#define AETHER_LIB_META_H

#ifdef __cplusplus
extern "C" {
#endif

/* One exported function. */
typedef struct {
    const char* aether_name;     /* "my_concat" or "std.fs.copy"        */
    const char* c_symbol;         /* unmangled C symbol callers dlsym    */
    const char* signature;        /* "(string, string) -> string"        */
    const char* source_file;      /* path of the .ae that defined it     */
    int         source_line;      /* 1-based                              */
} AetherLibFunction;

/* Top-level catalog. Stable layout — never reorder fields, only
 * append. New optional fields go at the end with a documented
 * "all-zero means absent" contract. */
typedef struct {
    const char* schema_version;   /* "1.0" — bump on breaking change      */
    const char* aether_version;   /* compiler version that produced this   */
    const char* primary_source;   /* the main .ae file passed to aetherc   */
    int                       function_count;
    const AetherLibFunction*  functions;
    int                       closure_count;   /* always 0 in v1; v2 fills */
    const void*               closures;        /* always NULL in v1        */
} AetherLibMeta;

/* The single entry point. Every `--emit=lib` artifact exports this
 * symbol; consumers `dlsym` for "aether_lib_meta" and call it.
 * Returns a pointer to a static (process-lifetime) struct — never
 * free, never modify. */
const AetherLibMeta* aether_lib_meta(void);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_LIB_META_H */
