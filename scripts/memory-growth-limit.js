/* Called by Emscripten before every growth, including allocator overgrowth.
 * The optional limit is the caller's remaining total-process allocation budget.
 * Invalid limits fail closed; absent hooks retain the compiled memory maximum. */
var terraCompiledHeapMaximum = getHeapMax;
getHeapMax = function() {
  var maximum = terraCompiledHeapMaximum();
  if (typeof Module['memoryGrowthLimit'] !== 'function') return maximum;
  var limit;
  try { limit = Module['memoryGrowthLimit'](); } catch (_) { return HEAPU8.length; }
  if (!Number.isFinite(limit) || limit < 0) return HEAPU8.length;
  return Math.min(maximum, Math.floor(limit / 65536) * 65536);
};
