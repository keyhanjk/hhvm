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

#include "hphp/runtime/vm/jit/irlower-internal.h"

#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/tv-conversions.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/arg-group.h"
#include "hphp/runtime/vm/jit/call-spec.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/util/trace.h"

namespace HPHP { namespace jit { namespace irlower {

TRACE_SET_MOD(irlower);

///////////////////////////////////////////////////////////////////////////////

/*
 * Attempt to convert `tv' to a given type, raising a warning and throwing a
 * TVCoercionException on failure.
 */
#define XX(kind, expkind)                                             \
void tvCoerceParamTo##kind##OrThrow(TypedValue* tv,                   \
                                    const Func* callee,               \
                                    unsigned int arg_num) {           \
  tvCoerceIfStrict(*tv, arg_num, callee);                             \
  if (LIKELY(tvCoerceParamTo##kind##InPlace(tv,                       \
                                            callee->isBuiltin()))) {  \
    return;                                                           \
  }                                                                   \
  raise_param_type_warning(callee->displayName()->data(),             \
                           arg_num, KindOf##expkind, tv->m_type);     \
  throw TVCoercionException(callee, arg_num, tv->m_type,              \
                            KindOf##expkind);                         \
}
#define X(kind) XX(kind, kind)
X(Boolean)
X(Int64)
X(Double)
X(String)
X(Vec)
X(Dict)
X(Keyset)
X(Array)
X(Object)
XX(NullableObject, Object)
X(Resource)
#undef X
#undef XX

///////////////////////////////////////////////////////////////////////////////

namespace {

void implCast(IRLS& env, const IRInstruction* inst, Vreg base, int offset) {
  auto type = inst->typeParam();
  auto nullable = false;

  if (!type.isKnownDataType()) {
    assertx(TNull <= type);
    type -= TNull;
    assertx(type.isKnownDataType());
    nullable = true;
  }
  assertx(IMPLIES(nullable, type <= TObj));

  auto const args = argGroup(env, inst).addr(base, offset);

  auto const helper = [&]() -> void (*)(TypedValue*) {
    if (type <= TBool) {
      return tvCastToBooleanInPlace;
    } else if (type <= TInt) {
      return tvCastToInt64InPlace;
    } else if (type <= TDbl) {
      return tvCastToDoubleInPlace;
    } else if (type <= TArr) {
      return tvCastToArrayInPlace;
    } else if (type <= TVec) {
      return tvCastToVecInPlace;
    } else if (type <= TDict) {
      return tvCastToDictInPlace;
    } else if (type <= TKeyset) {
      return tvCastToKeysetInPlace;
    } else if (type <= TStr) {
      return tvCastToStringInPlace;
    } else if (type <= TObj) {
      return nullable ? tvCastToNullableObjectInPlace : tvCastToObjectInPlace;
    } else if (type <= TRes) {
      return tvCastToResourceInPlace;
    } else {
      not_reached();
    }
  }();
  cgCallHelper(vmain(env), env, CallSpec::direct(helper),
               kVoidDest, SyncOptions::Sync, args);
}

void implCoerce(IRLS& env, const IRInstruction* inst,
                Vreg base, int offset, Func const* callee, int argNum) {
  auto const type = inst->typeParam();
  assertx(type.isKnownDataType());

  auto args = argGroup(env, inst)
    .addr(base, offset)
    .imm(callee)
    .imm(argNum);

  auto const helper = [&]()
    -> void (*)(TypedValue*, const Func*, unsigned int)
  {
    if (type <= TBool) {
      return tvCoerceParamToBooleanOrThrow;
    } else if (type <= TInt) {
      return tvCoerceParamToInt64OrThrow;
    } else if (type <= TDbl) {
      return tvCoerceParamToDoubleOrThrow;
    } else if (type <= TArr) {
      return tvCoerceParamToArrayOrThrow;
    } else if (type <= TVec) {
      return tvCoerceParamToVecOrThrow;
    } else if (type <= TDict) {
      return tvCoerceParamToDictOrThrow;
    } else if (type <= TKeyset) {
      return tvCoerceParamToKeysetOrThrow;
    } else if (type <= TStr) {
      return tvCoerceParamToStringOrThrow;
    } else if (type <= TObj) {
      return tvCoerceParamToObjectOrThrow;
    } else if (type <= TRes) {
      return tvCoerceParamToResourceOrThrow;
    } else {
      not_reached();
    }
  }();

  cgCallHelper(vmain(env), env, CallSpec::direct(helper),
               kVoidDest, SyncOptions::Sync, args);
}

}

void cgCastStk(IRLS& env, const IRInstruction *inst) {
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const offset = inst->extra<CastStk>()->offset;

  implCast(env, inst, sp, cellsToBytes(offset.offset));
}

void cgCastMem(IRLS& env, const IRInstruction *inst) {
  auto const ptr = srcLoc(env, inst, 0).reg();

  implCast(env, inst, ptr, 0);
}

void cgCoerceStk(IRLS& env, const IRInstruction *inst) {
  auto const extra = inst->extra<CoerceStk>();
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const offset = cellsToBytes(extra->offset.offset);

  implCoerce(env, inst, sp, offset, extra->callee, extra->argNum);
}

void cgCoerceMem(IRLS& env, const IRInstruction *inst) {
  auto const extra = inst->extra<CoerceMem>();
  auto const ptr = srcLoc(env, inst, 0).reg();

  implCoerce(env, inst, ptr, 0, extra->callee, extra->argNum);
}

IMPL_OPCODE_CALL(CoerceCellToBool);
IMPL_OPCODE_CALL(CoerceCellToInt);
IMPL_OPCODE_CALL(CoerceCellToDbl);
IMPL_OPCODE_CALL(CoerceStrToDbl);
IMPL_OPCODE_CALL(CoerceStrToInt);

///////////////////////////////////////////////////////////////////////////////

namespace {

void implVerifyCls(IRLS& env, const IRInstruction* inst) {
  auto const cls = inst->src(0);
  auto const constraint = inst->src(1);
  auto& v = vmain(env);

  if (cls->hasConstVal() && constraint->hasConstVal(TCls)) {
    if (cls->clsVal() != constraint->clsVal()) {
      cgCallNative(v, env, inst);
    }
    return;
  }

  auto const rcls = srcLoc(env, inst, 0).reg();
  auto const rconstraint = srcLoc(env, inst, 1).reg();
  auto const sf = v.makeReg();

  v << cmpq{rconstraint, rcls, sf};

  // The native call for this instruction is the slow path that does proper
  // subtype checking.  The comparisons above are just to short-circuit the
  // overhead when the Classes are an exact match.
  ifThen(v, CC_NE, sf, [&](Vout& v) { cgCallNative(v, env, inst); });
}

}

IMPL_OPCODE_CALL(VerifyParamCallable)
IMPL_OPCODE_CALL(VerifyRetCallable)
IMPL_OPCODE_CALL(VerifyParamFail)
IMPL_OPCODE_CALL(VerifyParamFailHard)
IMPL_OPCODE_CALL(VerifyRetFail)
IMPL_OPCODE_CALL(VerifyRetFailHard)

void cgVerifyParamCls(IRLS& env, const IRInstruction* inst) {
  implVerifyCls(env, inst);
}
void cgVerifyRetCls(IRLS& env, const IRInstruction* inst) {
  implVerifyCls(env, inst);
}

///////////////////////////////////////////////////////////////////////////////

static void hackArrParamNoticeImpl(const Func* f, const ArrayData* a,
                                   int64_t type, int64_t param) {
  raise_hackarr_type_hint_param_notice(f, a, AnnotType(type), param);
}

static void hackArrOutParamNoticeImpl(const Func* f, const ArrayData* a,
                                      int64_t type, int64_t param) {
  raise_hackarr_type_hint_outparam_notice(f, a, AnnotType(type), param);
}

static void hackArrRetNoticeImpl(const Func* f, const ArrayData* a,
                                 int64_t type) {
  raise_hackarr_type_hint_ret_notice(f, a, AnnotType(type));
}

void cgRaiseHackArrParamNotice(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<RaiseHackArrParamNotice>();

  auto args = argGroup(env, inst).ssa(1).ssa(0).imm(int64_t(extra->type));
  auto const target = [&] {
    if (extra->isReturn) {
      if (extra->id == HPHP::TypeConstraint::ReturnId) {
        return CallSpec::direct(hackArrRetNoticeImpl);
      }
      args.imm(extra->id);
      return CallSpec::direct(hackArrOutParamNoticeImpl);
    } else {
      args.imm(extra->id);
      return CallSpec::direct(hackArrParamNoticeImpl);
    }
  }();

  cgCallHelper(
    vmain(env),
    env,
    target,
    kVoidDest,
    SyncOptions::Sync,
    args
  );
}

///////////////////////////////////////////////////////////////////////////////

}}}
