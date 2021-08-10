#pragma once

#include <stdint.h>
#include <intrin.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* TODO(casey): 

   This is not designed to be a particularly high-speed cache.
   
   It is merely a simple example of a basic LRU cache, which is all 
   that is necessary for something like a terminal.
   
   Things that would probably want to be done for harder-core use:
   
   1) Consider and test some alternate cache designs to see if there
      are any that remain simple to understand, but provide better
      performance.  For example, this is a two-level cache (first the
      chain is looked up, then the elements), which almost certainly
      has worse cache behavior than a design where the first lookup
      produced an actual element.
      
   2) Battle-test all the functions with a lot of randomized and constructed
      data to ensure there are no lurking reference errors.  There are
      three chaining behaviors - the LRU chain, the hash chains, and the
      free list.  It would be nice to stress-test all of them to remove
      any remaining corner-case bugs.
*/

/* NOTE(casey):

   The glyph table requires some simple settings.
   
   HashCount = The size of the hash table.  This must be a power of two.
               Larger is faster, up to a point.  Values like 65536 may be
               appropriate for extensive codepoint usage, whereas much
               smaller values like 256 or 4096 would be fine for situations
               where very few Unicode combinations will be used.
               
   EntryCount = The total number of entries to remember.  This must be
                no larger than the number that fit into the actual cache
                texture, otherwise the glyph table will report back
                indexes into that texture that are "off the bottom".
                
   ReservedTileCount = The total number of rects in the cache texture
                       to reserve for direct mapping.  These will not
                       be used when assigning slots to hashes, and
                       are assumed to be for the app's own short-circuit
                       usage.
                       
   CacheTileCountInX = The number of rects to put horizontally in the
                       cache texture.  This should generally be the width of the
                       cache texture divided by the font width.
*/
typedef struct glyph_table_params
{
    uint32_t HashCount;
    uint32_t EntryCount;
    uint32_t ReservedTileCount;
    uint32_t CacheTileCountInX;
} glyph_table_params;

/* NOTE(casey):

   Glyph indices are packed Y.X as a 32-bit 16.16 value.  Whenever you get back a gpu_glyph_index,
   you can retrieve the X/Y ordinal of the point int he texture with UnpackGlyphCachePoint,
   so you don't have to do the unpacking yourself.
*/
typedef struct gpu_glyph_index
{
    uint32_t Value;
} gpu_glyph_index;
typedef struct glyph_cache_point
{
    uint32_t X, Y;
} glyph_cache_point;

/* NOTE(casey):

   To use the cache, compute a hash (however you want to, but it must be unique for your dataset)
   and pass it to FindGlyphEntryByHash.  You'll get back a glyph_state that represents
   this glyph in the cache.
*/
typedef struct glyph_hash
{
    __m128i Value;
} glyph_hash;
typedef struct glyph_state
{
    uint32_t ID;
    gpu_glyph_index GPUIndex;

    // NOTE(casey): Technically these two values can be whatever you want.
    uint32_t FilledState;
    uint16_t DimX;
    uint16_t DimY;
} glyph_state;

/* NOTE(casey):

   The table keeps some simple internal stats.  The values are zeroed after every GetAndClearStats,
   so the count is the total number since the last time the stats were retrieved.

   TODO(casey): This may be a mistake.  Perhaps the stats should just accumulate ad infinitum,
   and people can diff vs. their old stats to find out the change?
*/
typedef struct glyph_table_stats
{
    size_t HitCount; // NOTE(casey): Number of times FindGlyphEntryByHash hit the cache
    size_t MissCount; // NOTE(casey): Number of times FindGlyphEntryByHash misses the cache
    size_t RecycleCount;  // NOTE(casey): Number of times an entry had to be recycled to fill a cache miss
} glyph_table_stats;

// NOTE(Casey): "Opaque" types used for the internals:
typedef struct glyph_table glyph_table;
typedef struct glyph_entry glyph_entry;

/* NOTE(casey):

   If you have a non-zero ReservedTileCount, InitializeDirectGlyphTable
   will fill out a gpu_glyph_index array you provide with ReservedTileCount's
   worth of reserved rects you can use for direct-mapping codepoints of your
   choice (that you won't send to the cache).  This is independent of the
   glyph table, and does not change unless you change the glyph_table_params,
   even if you reallocate the glyph table.

   If you want an implicit 0 slot, the first slot filled by InitializeDirectGlyphTable
   is always guaranteed to be 0, so you can put it in your table if you wish.
   If instead you want it to skip writing it, because your table omits the 0 slot,
   you can pass 1 as SkipZeroSlot and it will start writing at Table[0] with the
   _first_ actual entry instead of the _zeroth_.

   I know that's confusing, but it was the only way I could think of to
   provide the ability for the user to both reserve the zero slot and not
   reserve the zero slot, their choice.
*/
void InitializeDirectGlyphTable(glyph_table_params Params, gpu_glyph_index* Table, int SkipZeroSlot);

/* NOTE(casey):

   To allocate a new glyph cache, call GetGlyphTableFootprint to find out the total size,
   allocate that, then pass the memory block to PlaceGlyphTableInMemory.
   Everything else is done for you:
   
   glyph_table *Table = PlaceGlyphTableInMemory(Params, malloc(GetGlyphTableFootprint(Params)));
   if(Table)
   {
       // ...
   }
   
   You do not need to check your allocation - PlaceGlyphTableInMemory will pass 0 through.
   So it is sufficient to just check the PlaceGlyphTableInMemory return value.
*/
size_t GetGlyphTableFootprint(glyph_table_params Params);
glyph_table *PlaceGlyphTableInMemory(glyph_table_params Params, void *Memory);

glyph_cache_point UnpackGlyphCachePoint(gpu_glyph_index P);

glyph_state FindGlyphEntryByHash(glyph_table *Table, glyph_hash RunHash);

/* NOTE(casey):

   Whenever you change the state of the cache texture, call UpdateGlyphCacheEntry with the ID from the glyph_state
   to inform the cache about the new filled status and tile count (if you are using tile counts).  Note that
   the cache itself doesn't care about this at all - it is strictly for your code, so you can retrieve tile counts
   later and/or know that glyphs have been sized and/or rasterized.
*/
void UpdateGlyphCacheEntry(glyph_table* Table, uint32_t ID, uint32_t NewState, uint16_t NewDimX, uint16_t NewDimY);

glyph_table_stats GetAndClearStats(glyph_table *Table);

#ifdef __cplusplus
}
#endif