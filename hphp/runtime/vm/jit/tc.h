/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#ifndef incl_HPHP_JIT_TC_H_
#define incl_HPHP_JIT_TC_H_

#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/code-cache.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/srcdb.h"
#include "hphp/runtime/vm/jit/stack-offsets.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/unique-stubs.h"
#include "hphp/util/growable-vector.h"

#include <folly/Optional.h>

#include <memory>
#include <string>
#include <vector>

namespace HPHP { namespace jit {

struct AsmInfo;
struct FPInvOffset;
struct IncomingBranch;
struct IRUnit;
struct ProfTransRec;
struct TransEnv;
struct TransLoc;
struct Vunit;

using OptView = folly::Optional<CodeCache::View>;

////////////////////////////////////////////////////////////////////////////////

namespace tc {

struct TransRange {
  TcaRange main;
  TcaRange cold;
  TcaRange frozen;
  TcaRange data;

  TransLoc loc() const;
};

using CodeViewPtr = std::unique_ptr<CodeCache::View>;

struct TransMetaInfo {
  SrcKey sk;
  CodeCache::View emitView; // View code was emitted into (may be thread local)
  TransKind   viewKind; // TransKind used to select code view
  TransKind   transKind; // TransKind used for translation
  TransRange  range;
  CodeViewPtr finalView; // View where code finally ended up (after relocation)
  TransLoc    loc; // final location of translation (after relocation)
  CGMeta      meta;
  TransRec    transRec;
  GrowableVector<IncomingBranch> tailBranches;
};

struct PrologueMetaInfo {
  PrologueMetaInfo(ProfTransRec* rec)
    : transRec(rec)
  { }
  ProfTransRec* transRec{nullptr};
  TransID       transID{kInvalidTransID};
  TCA           start{0};
  TransLoc      loc;
  CGMeta        meta;
};

struct BodyDispatchMetaInfo {
  BodyDispatchMetaInfo(TCA tca, CodeCache::View view)
    : tca(tca), finalView(view)
  { }
  TCA             tca;
  CodeCache::View finalView;
};

struct LocalTCBuffer {
  LocalTCBuffer() = default;
  explicit LocalTCBuffer(Address start, size_t initialSize);
  LocalTCBuffer(LocalTCBuffer&&) = default;
  LocalTCBuffer& operator=(LocalTCBuffer&&) = default;

  OptView view();
  bool valid() const { return m_main.base() != nullptr; }

private:
  CodeBlock m_main;
  CodeBlock m_cold;
  CodeBlock m_frozen;
  DataBlock m_data;
};

struct FuncMetaInfo {
  enum class Kind : uint8_t {
    Prologue,
    Translation,
  };

  FuncMetaInfo() = default;
  FuncMetaInfo(Func* f, LocalTCBuffer&& buf)
    : fid(f->getFuncId())
    , func(f)
    , tcBuf(std::move(buf))
  {}

  FuncMetaInfo(FuncMetaInfo&&) = default;
  FuncMetaInfo& operator=(FuncMetaInfo&&) = default;

  FuncId fid;
  Func* func;
  LocalTCBuffer tcBuf;

  void add(ProfTransRec* p) {
    prologues.emplace_back(p);
    order.emplace_back(Kind::Prologue);
  }

  void add(TransMetaInfo&& t) {
    translations.emplace_back(std::move(t));
    order.emplace_back(Kind::Translation);
  }

  // We rebuild a variant type here because using boosts fails on opensource
  // builds because it at some point requires a copy construction.
  // This vector has one entry per prologue/translation stored in the two
  // vectors above, and it encodes the order in which they should be published.
  std::vector<Kind> order;

  std::unique_ptr<BodyDispatchMetaInfo> bodyDispatch;
  std::vector<PrologueMetaInfo>         prologues;
  std::vector<TransMetaInfo>            translations;
};

////////////////////////////////////////////////////////////////////////////////

/*
 * Returns true iff we already have Eval.JitMaxTranslations translations
 * recorded in srcRec.
 */
bool reachedTranslationLimit(TransKind kind, SrcKey sk, const SrcRec& srcRec);

/*
 * Emit machine code for env. Returns folly::none if the global translation
 * limit has been reached, generates an interp request if vunit is null or
 * codegen fails. If optDst is set it must be a thread local view and the
 * code lock will not be acquired while writing to it.
 *
 * The resulting translation will not yet be live.
 */
folly::Optional<TransMetaInfo> emitTranslation(
  TransEnv env,
  OptView optDst = folly::none
);

/*
 * Make a translation generated by emitTranslation live. If the translation was
 * emitted into a per-thread buffer then optSrcView must be the view into which
 * it was emitted, it will be relocated at the end of the live TC.
 */
folly::Optional<TransLoc> publishTranslation(
  TransMetaInfo info,
  OptView optSrcView = folly::none
);

/*
 * Publish a set of optimized translations associated with a particular
 * function. It is assumed that these translations have been emitted into per-
 * thread buffers and will need to be relocated.
 */
void publishOptFunc(FuncMetaInfo info);

/*
 * Acquires the code and metadata locks once, and then processes all the
 * functions in `infos' by:
 *  1) relocating their translations into the TC in the order given by `infos';
 *  2) smashing all the calls and jumps between these translations;
 *  3) optimizing the calls and jumps smashed in step 2);
 *  4) publishing these translations.
 */
void relocatePublishSortedOptFuncs(std::vector<FuncMetaInfo> infos);

/*
 * Emit a new prologue for func-- returns nullptr if the global translation
 * limit has been reached.
 */
TCA emitFuncPrologue(Func* func, int argc, TransKind kind);

/*
 * Emits an optimized prologue for rec.
 *
 * Smashes the callers of the prologue for rec and updates the cached func
 * prologue.
 */
void emitFuncPrologueOpt(ProfTransRec* rec);

/*
 * Emit the prologue dispatch for func which contains dvs DV initializers, and
 * return its start address.  The `kind' of translation argument is used to
 * decide what area of the code cache will be used (hot, main, or prof).
 */
TCA emitFuncBodyDispatch(Func* func, const DVFuncletsVec& dvs, TransKind kind);

////////////////////////////////////////////////////////////////////////////////

/*
 * True iff the global translation limit has not yet been reached.
 */
bool canTranslate();

/*
 * Whether we should emit a translation of kind for func, ignoring the cap on
 * overall TC size.
 */
bool shouldTranslateNoSizeLimit(const Func* func, TransKind kind);

/*
 * Whether we should emit a translation of kind for func.
 */
bool shouldTranslate(const Func* func, TransKind kind);

/*
 * Whether we are still profiling new functions.
 */
bool shouldProfileNewFuncs();

/*
 * Whether we should try profile-guided optimization when translating `func'.
 */
bool profileFunc(const Func* func);

/*
 * Attempt to discard profData via the treadmill if it is no longer needed.
 */
void checkFreeProfData();

////////////////////////////////////////////////////////////////////////////////

/*
 * SrcRec for sk or nullptr if no SrcRec has been created.
 */
SrcRec* findSrcRec(SrcKey sk);

/*
 * Create a SrcRec for sk with an sp offset of spOff.
 */
void createSrcRec(SrcKey sk, FPInvOffset spOff);

////////////////////////////////////////////////////////////////////////////////

/*
 * Assert ownership of the CodeCache by this thread.
 *
 * Must be held even if the current thread owns the global write lease.
 */
void assertOwnsCodeLock(OptView v = folly::none);

/*
 * Assert ownership of the tc metadata by this thread.
 *
 * Must be held even if the current thread owns the global write lease.
 */
void assertOwnsMetadataLock();

////////////////////////////////////////////////////////////////////////////////

/*
 * Get the table of unique stubs
 *
 * Pre: processInit()
 */
ALWAYS_INLINE const UniqueStubs& ustubs() {
  extern UniqueStubs g_ustubs;
  return g_ustubs;
}

/*
 * Perform one time process initialization for the structures associated with
 * this module.
 */
void processInit();

/*
 * Perform process shutdown functions for the TC including joining any
 * outstanding background threads.
 */
void processExit();

////////////////////////////////////////////////////////////////////////////////

/*
 * Perform TC related request initialization and teardown.
 */
void requestInit();
void requestExit();

/*
 * Returns the total size of the TC now and at the beginning of this request,
 * in bytes. Note that the code may have been emitted by other threads.
 *
 * Pre: requestInit()
 */
void codeEmittedThisRequest(size_t& requestEntry, size_t& now);

////////////////////////////////////////////////////////////////////////////////

/*
 * Reclaim all TC space associated with func
 *
 * Before any space is reclaimed the following actions will be performed:
 *  (1) Smash all prologues
 *  (2) Smash all callers to bind-call unique stubs
 *  (3) Erase all call meta-data for calls into function
 *
 * After all calls and prologues have been smashed any on-going requests will be
 * allowed to complete before TC Space will be reclaimed for:
 *  (1) All prologues
 *  (2) All translations
 *  (3) All anchor translations
 *
 * This function should only be called from Func::destroy() and may access the
 * fullname and ID of the function.
 */
void reclaimFunction(const Func* func);

/*
 * Allows TC space for translations in trans to be reused in future
 * translations.
 *
 * Reclaiming a translation will:
 *  (1) Mark bytes available for reuse in the code-blocks associated with
 *      the translation
 *  (2) Erase any IBs from translation into other SrcRecs
 *  (3) Erase any jump annotations in MCGenerator used to generate optimized
 *      traces
 *  (4) Erase an metadata about smashed calls in the translation from both the
 *      reuse-tc module and the prof-data module
 *
 * The translation _must_ be unreachable before reclaimTranslation() is called,
 * this is generally done by calling reclaimFunction() or performing
 * replaceOldTranslations() on a SrcRec
 */
void reclaimTranslations(GrowableVector<TransLoc>&& trans);

/*
 * Free an ephemeral stub.
 */
void freeTCStub(TCA stub);

////////////////////////////////////////////////////////////////////////////////

/*
 * Emit checks for (and hooks into) an attached debugger in front of each
 * translation in `unit' or for `SrcKey{func, offset, resumed}'.
 */
bool addDbgGuards(const Func* func);
bool addDbgGuard(const Func* func, Offset offset, ResumeMode resumeMode);

////////////////////////////////////////////////////////////////////////////////

struct UsageInfo {
  std::string name;
  size_t used;
  size_t capacity;
  bool global;
};

/*
 * Get UsageInfo data for all the TC code sections, including global data, and
 * also for RDS.
 */
std::vector<UsageInfo> getUsageInfo();

/*
 * Like getUsageInfo(), but formatted as a pleasant string.
 */
std::string getTCSpace();

/*
 * Return a string containing the names and start addresses of all TC code
 * sections.
 */
std::string getTCAddrs();

/*
 * Dump the translation cache to files in RuntimeOption::EvalDumpTCPath
 * (default: /tmp), returning success.
 */
bool dump(bool ignoreLease = false);

struct TCMemInfo {
  std::string name;
  size_t used;
  size_t allocs;
  size_t frees;
  size_t free_size;
  size_t free_blocks;
};

/*
 * Get per section memory usage data for the TC.
 */
std::vector<TCMemInfo> getTCMemoryUsage();

////////////////////////////////////////////////////////////////////////////////

/*
 * Convert between TC addresses and offsets.
 */
extern CodeCache* g_code;
ALWAYS_INLINE TCA offsetToAddr(uint32_t off) {
  return g_code->toAddr(off);
}
ALWAYS_INLINE uint32_t addrToOffset(CTCA addr) {
  return g_code->toOffset(addr);
}

/*
 * Check if `addr' is an address within the TC.
 */
bool isValidCodeAddress(TCA addr);

/*
 * Check if `addr' is an address within the profile code block in the TC.
 */
bool isProfileCodeAddress(TCA addr);

////////////////////////////////////////////////////////////////////////////////

/*
  relocate using data from perf.
  If time is non-negative, its used as the time to run perf record.
  If time is -1, we pick a random subset of translations, and relocate them
  in a random order.
  If time is -2, we relocate all of the translations.

  Currently we don't ever relocate anything from frozen (or prof). We also
  don't relocate the cold portion of translations; but we still need to know
  where those are in order to relocate back-references to the code that was
  relocated.
*/
void liveRelocate(int time);

inline void liveRelocate(bool random) {
  return liveRelocate(random ? -1 : 20);
}

/*
 * Relocate a new translation to the current frontiers of main and cold. Code
 * in frozen is not moved.
 *
 * TODO(t10543562): This can probably be merged with relocateNewTranslation.
 */
void relocateTranslation(
  const IRUnit* unit,
  CodeBlock& main, CodeBlock& main_in, CodeAddress main_start,
  CodeBlock& cold, CodeBlock& cold_in, CodeAddress cold_start,
  CodeBlock& frozen, CodeAddress frozen_start,
  AsmInfo* ai, CGMeta& meta
);

/*
 * Record data for live relocations.
 */
void recordPerfRelocMap(
  TCA start, TCA end,
  TCA coldStart, TCA coldEnd,
  SrcKey sk, int argNum,
  const GrowableVector<IncomingBranch> &incomingBranches,
  CGMeta& fixups);

////////////////////////////////////////////////////////////////////////////////

/*
 * Information about the number of bound calls, branches, and tracked functions
 * for use in logging.
 */
int smashedCalls();
int smashedBranches();
int recordedFuncs();

/*
 * Record a jmp at address toSmash to SrcRec sr.
 *
 * When a translation is reclaimed we remove all annotations from all SrcRecs
 * containing IBs from the translation so that they cannot be inadvertently
 * smashed in the process of replaceOldTranslations()
 */
void recordJump(TCA toSmash, SrcRec* sr);

/*
 * Insert a jump to destSk at toSmash. If no top translation for destSk exists
 * no action is performed. On return, the value of smashed indicated whether a
 * new address was written into the TC.
 */
TCA bindJmp(TCA toSmash, SrcKey destSk, TransFlags trflags, bool& smashed);

/*
 * Insert the address for branches to destSk at toSmash. Upon return, the value
 * of smashed indicates whether an address was written into the TC.
 */
TCA bindAddr(TCA toSmash, SrcKey destSk, TransFlags trflags, bool& smashed);

/*
 * Bind a call to start at toSmash, where start is the prologue for callee, when
 * invoked with nArgs.
 */
void bindCall(TCA toSmash, TCA start, Func* callee, int nArgs, bool immutable);

}}}

#endif
