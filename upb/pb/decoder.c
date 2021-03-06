/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * Copyright (c) 2008-2013 Google Inc.  See LICENSE for details.
 * Author: Josh Haberman <jhaberman@gmail.com>
 *
 * This file implements a VM for the interpreted (bytecode) decoder.
 *
 * Bytecode must previously have been generated using the bytecode compiler in
 * compile_decoder.c.  This decoder then walks through the bytecode op-by-op to
 * parse the input.
 *
 * Decoding is fully resumable; we just keep a pointer to the current bytecode
 * instruction and resume from there.  A fair amount of the logic here is to
 * handle the fact that values can span buffer seams and we have to be able to
 * be capable of suspending/resuming from any byte in the stream.  This
 * sometimes requires keeping a few trailing bytes from the last buffer around
 * in the "residual" buffer.
 */

#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include "upb/pb/decoder.int.h"
#include "upb/pb/varint.int.h"

#ifdef UPB_DUMP_BYTECODE
#include <stdio.h>
#endif

#define CHECK_SUSPEND(x) if (!(x)) return upb_pbdecoder_suspend(d);

// Error messages that are shared between the bytecode and JIT decoders.
const char *kPbDecoderStackOverflow = "Nesting too deep.";

// Error messages shared within this file.
static const char *kUnterminatedVarint = "Unterminated varint.";

/* upb_pbdecoder **************************************************************/

static opcode halt = OP_HALT;

// Whether an op consumes any of the input buffer.
static bool consumes_input(opcode op) {
  switch (op) {
    case OP_SETDISPATCH:
    case OP_STARTMSG:
    case OP_ENDMSG:
    case OP_STARTSEQ:
    case OP_ENDSEQ:
    case OP_STARTSUBMSG:
    case OP_ENDSUBMSG:
    case OP_STARTSTR:
    case OP_ENDSTR:
    case OP_PUSHTAGDELIM:
    case OP_POP:
    case OP_SETDELIM:
    case OP_SETBIGGROUPNUM:
    case OP_CHECKDELIM:
    case OP_CALL:
    case OP_RET:
    case OP_BRANCH:
      return false;
    default:
      return true;
  }
}

static bool in_residual_buf(const upb_pbdecoder *d, const char *p);

// It's unfortunate that we have to micro-manage the compiler this way,
// especially since this tuning is necessarily specific to one hardware
// configuration.  But emperically on a Core i7, performance increases 30-50%
// with these annotations.  Every instance where these appear, gcc 4.2.1 made
// the wrong decision and degraded performance in benchmarks.
#define FORCEINLINE static inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))

static void seterr(upb_pbdecoder *d, const char *msg) {
  // TODO(haberman): encapsulate this access to pipeline->status, but not sure
  // exactly what that interface should look like.
  upb_status_seterrmsg(d->status, msg);
}

void upb_pbdecoder_seterr(upb_pbdecoder *d, const char *msg) {
  seterr(d, msg);
}


/* Buffering ******************************************************************/

// We operate on one buffer at a time, which is either the user's buffer passed
// to our "decode" callback or some residual bytes from the previous buffer.

// How many bytes can be safely read from d->ptr without reading past end-of-buf
// or past the current delimited end.
static size_t curbufleft(const upb_pbdecoder *d) {
  assert(d->data_end >= d->ptr);
  return d->data_end - d->ptr;
}

// Overall stream offset of d->ptr.
uint64_t offset(const upb_pbdecoder *d) {
  return d->bufstart_ofs + (d->ptr - d->buf);
}

// Advances d->ptr.
static void advance(upb_pbdecoder *d, size_t len) {
  assert(curbufleft(d) >= len);
  d->ptr += len;
}

static bool in_buf(const char *p, const char *buf, const char *end) {
  return p >= buf && p <= end;
}

static bool in_residual_buf(const upb_pbdecoder *d, const char *p) {
  return in_buf(p, d->residual, d->residual_end);
}

// Calculates the delim_end value, which is affected by both the current buffer
// and the parsing stack, so must be called whenever either is updated.
static void set_delim_end(upb_pbdecoder *d) {
  size_t delim_ofs = d->top->end_ofs - d->bufstart_ofs;
  if (delim_ofs <= (d->end - d->buf)) {
    d->delim_end = d->buf + delim_ofs;
    d->data_end = d->delim_end;
  } else {
    d->data_end = d->end;
    d->delim_end = NULL;
  }
}

static void switchtobuf(upb_pbdecoder *d, const char *buf, const char *end) {
  d->ptr = buf;
  d->buf = buf;
  d->end = end;
  set_delim_end(d);
}

static void advancetobuf(upb_pbdecoder *d, const char *buf, size_t len) {
  assert(curbufleft(d) == 0);
  d->bufstart_ofs += (d->end - d->buf);
  switchtobuf(d, buf, buf + len);
}

static void checkpoint(upb_pbdecoder *d) {
  // The assertion here is in the interests of efficiency, not correctness.
  // We are trying to ensure that we don't checkpoint() more often than
  // necessary.
  assert(d->checkpoint != d->ptr);
  d->checkpoint = d->ptr;
}

// Resumes the decoder from an initial state or from a previous suspend.
int32_t upb_pbdecoder_resume(upb_pbdecoder *d, void *p, const char *buf,
                             size_t size, const upb_bufhandle *handle) {
  UPB_UNUSED(p);  // Useless; just for the benefit of the JIT.
  d->buf_param = buf;
  d->size_param = size;
  d->handle = handle;
  if (d->residual_end > d->residual) {
    // We have residual bytes from the last buffer.
    assert(d->ptr == d->residual);
  } else {
    switchtobuf(d, buf, buf + size);
  }
  d->checkpoint = d->ptr;
  if (d->top->groupnum < 0) {
    CHECK_RETURN(upb_pbdecoder_skipunknown(d, -1, 0));
    d->checkpoint = d->ptr;
  }
  return DECODE_OK;
}

// Suspends the decoder at the last checkpoint, without saving any residual
// bytes.  If there are any unconsumed bytes, returns a short byte count.
size_t upb_pbdecoder_suspend(upb_pbdecoder *d) {
  d->pc = d->last;
  if (d->checkpoint == d->residual) {
    // Checkpoint was in residual buf; no user bytes were consumed.
    d->ptr = d->residual;
    return 0;
  } else {
    assert(!in_residual_buf(d, d->checkpoint));
    assert(d->buf == d->buf_param);
    size_t consumed = d->checkpoint - d->buf;
    d->bufstart_ofs += consumed;
    d->residual_end = d->residual;
    switchtobuf(d, d->residual, d->residual_end);
    return consumed;
  }
}

// Suspends the decoder at the last checkpoint, and saves any unconsumed
// bytes in our residual buffer.  This is necessary if we need more user
// bytes to form a complete value, which might not be contiguous in the
// user's buffers.  Always consumes all user bytes.
static size_t suspend_save(upb_pbdecoder *d) {
  // We hit end-of-buffer before we could parse a full value.
  // Save any unconsumed bytes (if any) to the residual buffer.
  d->pc = d->last;

  if (d->checkpoint == d->residual) {
    // Checkpoint was in residual buf; append user byte(s) to residual buf.
    assert((d->residual_end - d->residual) + d->size_param <=
           sizeof(d->residual));
    if (!in_residual_buf(d, d->ptr)) {
      d->bufstart_ofs -= (d->residual_end - d->residual);
    }
    memcpy(d->residual_end, d->buf_param, d->size_param);
    d->residual_end += d->size_param;
  } else {
    // Checkpoint was in user buf; old residual bytes not needed.
    assert(!in_residual_buf(d, d->checkpoint));
    d->ptr = d->checkpoint;
    size_t save = curbufleft(d);
    assert(save <= sizeof(d->residual));
    memcpy(d->residual, d->ptr, save);
    d->residual_end = d->residual + save;
    d->bufstart_ofs = offset(d);
  }

  switchtobuf(d, d->residual, d->residual_end);
  return d->size_param;
}

// Skips "bytes" bytes in the stream, which may be more than available.  If we
// skip more bytes than are available, we return a long read count to the caller
// indicating how many bytes the caller should skip before passing a new buffer.
static int32_t skip(upb_pbdecoder *d, size_t bytes) {
  assert(!in_residual_buf(d, d->ptr) || d->size_param == 0);
  if (curbufleft(d) >= bytes) {
    // Skipped data is all in current buffer.
    advance(d, bytes);
    return DECODE_OK;
  } else {
    // Skipped data extends beyond currently available buffers.
    d->pc = d->last;
    size_t skip = bytes - curbufleft(d);
    d->bufstart_ofs += (d->end - d->buf) + skip;
    d->residual_end = d->residual;
    switchtobuf(d, d->residual, d->residual_end);
    return d->size_param + skip;
  }
}

// Copies the next "bytes" bytes into "buf" and advances the stream.
// Requires that this many bytes are available in the current buffer.
FORCEINLINE void consumebytes(upb_pbdecoder *d, void *buf, size_t bytes) {
  assert(bytes <= curbufleft(d));
  memcpy(buf, d->ptr, bytes);
  advance(d, bytes);
}

// Slow path for getting the next "bytes" bytes, regardless of whether they are
// available in the current buffer or not.  Returns a status code as described
// in decoder.int.h.
static NOINLINE int32_t getbytes_slow(upb_pbdecoder *d, void *buf,
                                      size_t bytes) {
  const size_t avail = curbufleft(d);
  consumebytes(d, buf, avail);
  bytes -= avail;
  assert(bytes > 0);
  if (in_residual_buf(d, d->ptr)) {
    advancetobuf(d, d->buf_param, d->size_param);
  }
  if (curbufleft(d) >= bytes) {
    consumebytes(d, buf + avail, bytes);
    return DECODE_OK;
  } else if (d->data_end == d->delim_end) {
    seterr(d, "Submessage ended in the middle of a value or group");
    return upb_pbdecoder_suspend(d);
  } else {
    return suspend_save(d);
  }
}

// Gets the next "bytes" bytes, regardless of whether they are available in the
// current buffer or not.  Returns a status code as described in decoder.int.h.
FORCEINLINE int32_t getbytes(upb_pbdecoder *d, void *buf, size_t bytes) {
  if (curbufleft(d) >= bytes) {
    // Buffer has enough data to satisfy.
    consumebytes(d, buf, bytes);
    return DECODE_OK;
  } else {
    return getbytes_slow(d, buf, bytes);
  }
}

static NOINLINE size_t peekbytes_slow(upb_pbdecoder *d, void *buf,
                                      size_t bytes) {
  size_t ret = curbufleft(d);
  memcpy(buf, d->ptr, ret);
  if (in_residual_buf(d, d->ptr)) {
    size_t copy = UPB_MIN(bytes - ret, d->size_param);
    memcpy(buf + ret, d->buf_param, copy);
    ret += copy;
  }
  return ret;
}

FORCEINLINE size_t peekbytes(upb_pbdecoder *d, void *buf, size_t bytes) {
  if (curbufleft(d) >= bytes) {
    memcpy(buf, d->ptr, bytes);
    return bytes;
  } else {
    return peekbytes_slow(d, buf, bytes);
  }
}


/* Decoding of wire types *****************************************************/

// Slow path for decoding a varint from the current buffer position.
// Returns a status code as described in decoder.int.h.
NOINLINE int32_t upb_pbdecoder_decode_varint_slow(upb_pbdecoder *d,
                                                  uint64_t *u64) {
  *u64 = 0;
  uint8_t byte = 0x80;
  int bitpos;
  for(bitpos = 0; bitpos < 70 && (byte & 0x80); bitpos += 7) {
    int32_t ret = getbytes(d, &byte, 1);
    if (ret >= 0) return ret;
    *u64 |= (uint64_t)(byte & 0x7F) << bitpos;
  }
  if(bitpos == 70 && (byte & 0x80)) {
    seterr(d, kUnterminatedVarint);
    return upb_pbdecoder_suspend(d);
  }
  return DECODE_OK;
}

// Decodes a varint from the current buffer position.
// Returns a status code as described in decoder.int.h.
FORCEINLINE int32_t decode_varint(upb_pbdecoder *d, uint64_t *u64) {
  if (curbufleft(d) > 0 && !(*d->ptr & 0x80)) {
    *u64 = *d->ptr;
    advance(d, 1);
    return DECODE_OK;
  } else if (curbufleft(d) >= 10) {
    // Fast case.
    upb_decoderet r = upb_vdecode_fast(d->ptr);
    if (r.p == NULL) {
      seterr(d, kUnterminatedVarint);
      return upb_pbdecoder_suspend(d);
    }
    advance(d, r.p - d->ptr);
    *u64 = r.val;
    return DECODE_OK;
  } else {
    // Slow case -- varint spans buffer seam.
    return upb_pbdecoder_decode_varint_slow(d, u64);
  }
}

// Decodes a 32-bit varint from the current buffer position.
// Returns a status code as described in decoder.int.h.
FORCEINLINE int32_t decode_v32(upb_pbdecoder *d, uint32_t *u32) {
  uint64_t u64;
  int32_t ret = decode_varint(d, &u64);
  if (ret >= 0) return ret;
  if (u64 > UINT32_MAX) {
    seterr(d, "Unterminated 32-bit varint");
    // TODO(haberman) guarantee that this function return is >= 0 somehow,
    // so we know this path will always be treated as error by our caller.
    // Right now the size_t -> int32_t can overflow and produce negative values.
    *u32 = 0;
    return upb_pbdecoder_suspend(d);
  }
  *u32 = u64;
  return DECODE_OK;
}

// Decodes a fixed32 from the current buffer position.
// Returns a status code as described in decoder.int.h.
// TODO: proper byte swapping for big-endian machines.
FORCEINLINE int32_t decode_fixed32(upb_pbdecoder *d, uint32_t *u32) {
  return getbytes(d, u32, 4);
}

// Decodes a fixed64 from the current buffer position.
// Returns a status code as described in decoder.int.h.
// TODO: proper byte swapping for big-endian machines.
FORCEINLINE int32_t decode_fixed64(upb_pbdecoder *d, uint64_t *u64) {
  return getbytes(d, u64, 8);
}

// Non-static versions of the above functions.
// These are called by the JIT for fallback paths.
int32_t upb_pbdecoder_decode_f32(upb_pbdecoder *d, uint32_t *u32) {
  return decode_fixed32(d, u32);
}

int32_t upb_pbdecoder_decode_f64(upb_pbdecoder *d, uint64_t *u64) {
  return decode_fixed64(d, u64);
}

static double as_double(uint64_t n) { double d; memcpy(&d, &n, 8); return d; }
static float  as_float(uint32_t n)  { float  f; memcpy(&f, &n, 4); return f; }

// Pushes a frame onto the decoder stack.
static bool decoder_push(upb_pbdecoder *d, uint64_t end) {
  upb_pbdecoder_frame *fr = d->top;

  if (end > fr->end_ofs) {
    seterr(d, "Submessage end extends past enclosing submessage.");
    return false;
  } else if ((fr + 1) == d->limit) {
    seterr(d, kPbDecoderStackOverflow);
    return false;
  }

  fr++;
  fr->end_ofs = end;
  fr->dispatch = NULL;
  fr->groupnum = 0;
  d->top = fr;
  return true;
}

static bool pushtagdelim(upb_pbdecoder *d, uint32_t arg) {
  // While we expect to see an "end" tag (either ENDGROUP or a non-sequence
  // field number) prior to hitting any enclosing submessage end, pushing our
  // existing delim end prevents us from continuing to parse values from a
  // corrupt proto that doesn't give us an END tag in time.
  if (!decoder_push(d, d->top->end_ofs))
    return false;
  d->top->groupnum = arg;
  return true;
}

// Pops a frame from the decoder stack.
static void decoder_pop(upb_pbdecoder *d) { d->top--; }

NOINLINE int32_t upb_pbdecoder_checktag_slow(upb_pbdecoder *d,
                                             uint64_t expected) {
  uint64_t data = 0;
  size_t bytes = upb_value_size(expected);
  size_t read = peekbytes(d, &data, bytes);
  if (read == bytes && data == expected) {
    // Advance past matched bytes.
    int32_t ok = getbytes(d, &data, read);
    UPB_ASSERT_VAR(ok, ok < 0);
    return DECODE_OK;
  } else if (read < bytes && memcmp(&data, &expected, read) == 0) {
    return suspend_save(d);
  } else {
    return DECODE_MISMATCH;
  }
}

int32_t upb_pbdecoder_skipunknown(upb_pbdecoder *d, int32_t fieldnum,
                                  uint8_t wire_type) {
  if (fieldnum >= 0)
    goto have_tag;

  while (true) {
    uint32_t tag;
    CHECK_RETURN(decode_v32(d, &tag));
    wire_type = tag & 0x7;
    fieldnum = tag >> 3;

have_tag:
    if (fieldnum == 0) {
      seterr(d, "Saw invalid field number (0)");
      return upb_pbdecoder_suspend(d);
    }

    // TODO: deliver to unknown field callback.
    switch (wire_type) {
      case UPB_WIRE_TYPE_32BIT:
        CHECK_RETURN(skip(d, 4));
        break;
      case UPB_WIRE_TYPE_64BIT:
        CHECK_RETURN(skip(d, 8));
        break;
      case UPB_WIRE_TYPE_VARINT: {
        uint64_t u64;
        CHECK_RETURN(decode_varint(d, &u64));
        break;
      }
      case UPB_WIRE_TYPE_DELIMITED: {
        uint32_t len;
        CHECK_RETURN(decode_v32(d, &len));
        CHECK_RETURN(skip(d, len));
        break;
      }
      case UPB_WIRE_TYPE_START_GROUP:
        CHECK_SUSPEND(pushtagdelim(d, -fieldnum));
        break;
      case UPB_WIRE_TYPE_END_GROUP:
        if (fieldnum == -d->top->groupnum) {
          decoder_pop(d);
        } else if (fieldnum == d->top->groupnum) {
          return DECODE_ENDGROUP;
        } else {
          seterr(d, "Unmatched ENDGROUP tag.");
          return upb_pbdecoder_suspend(d);
        }
        break;
      default:
        seterr(d, "Invalid wire type");
        return upb_pbdecoder_suspend(d);
    }

    if (d->top->groupnum >= 0) {
      return DECODE_OK;
    }

    if (d->ptr == d->delim_end) {
      seterr(d, "Enclosing submessage ended in the middle of value or group");
      // Unlike most errors we notice during parsing, right now we have consumed
      // all of the user's input.
      //
      // There are three different options for how to handle this case:
      //
      //   1. decode() = short count, error = set
      //   2. decode() = full count, error = set
      //   3. decode() = full count, error NOT set, short count and error will
      //      be reported on next call to decode() (or end())
      //
      // (1) and (3) have the advantage that they preserve the invariant that an
      // error occurs iff decode() returns a short count.
      //
      // (2) and (3) have the advantage of reflecting the fact that all of the
      // bytes were in fact parsed (and possibly delivered to the unknown field
      // handler, in the future when that is supported).
      //
      // (3) requires extra state in the decode (a place to store the "permanent
      // error" that we should return for all subsequent attempts to decode).
      // But we likely want this anyway.
      //
      // Right now we do (1), thanks to the fact that we checkpoint *after* this
      // check.  (3) may be a better choice long term; unclear at the moment.
      return upb_pbdecoder_suspend(d);
    }

    checkpoint(d);
  }
}

static void goto_endmsg(upb_pbdecoder *d) {
  upb_value v;
  bool found = upb_inttable_lookup32(d->top->dispatch, DISPATCH_ENDMSG, &v);
  UPB_ASSERT_VAR(found, found);
  d->pc = d->top->base + upb_value_getuint64(v);
}

// Parses a tag and jumps to the corresponding bytecode instruction for this
// field.
//
// If the tag is unknown (or the wire type doesn't match), parses the field as
// unknown.  If the tag is a valid ENDGROUP tag, jumps to the bytecode
// instruction for the end of message.
static int32_t dispatch(upb_pbdecoder *d) {
  upb_inttable *dispatch = d->top->dispatch;

  // Decode tag.
  uint32_t tag;
  CHECK_RETURN(decode_v32(d, &tag));
  uint8_t wire_type = tag & 0x7;
  uint32_t fieldnum = tag >> 3;

  // Lookup tag.  Because of packed/non-packed compatibility, we have to
  // check the wire type against two possibilities.
  upb_value val;
  if (fieldnum != DISPATCH_ENDMSG &&
      upb_inttable_lookup32(dispatch, fieldnum, &val)) {
    uint64_t v = upb_value_getuint64(val);
    if (wire_type == (v & 0xff)) {
      d->pc = d->top->base + (v >> 16);
      return DECODE_OK;
    } else if (wire_type == ((v >> 8) & 0xff)) {
      bool found =
          upb_inttable_lookup(dispatch, fieldnum + UPB_MAX_FIELDNUMBER, &val);
      UPB_ASSERT_VAR(found, found);
      d->pc = d->top->base + upb_value_getuint64(val);
      return DECODE_OK;
    }
  }

  // Unknown field or ENDGROUP.
  int32_t ret = upb_pbdecoder_skipunknown(d, fieldnum, wire_type);

  if (ret == DECODE_ENDGROUP) {
    goto_endmsg(d);
    return DECODE_OK;
  } else {
    d->pc = d->last - 1;  // Rewind to CHECKDELIM.
    return ret;
  }
}

// Callers know that the stack is more than one deep because the opcodes that
// call this only occur after PUSH operations.
upb_pbdecoder_frame *outer_frame(upb_pbdecoder *d) {
  assert(d->top != d->stack);
  return d->top - 1;
}


/* The main decoding loop *****************************************************/

// The main decoder VM function.  Uses traditional bytecode dispatch loop with a
// switch() statement.
size_t upb_pbdecoder_decode(void *closure, const void *hd, const char *buf,
                            size_t size, const upb_bufhandle *handle) {
  upb_pbdecoder *d = closure;
  const mgroup *group = hd;
  assert(buf);
  int32_t result = upb_pbdecoder_resume(d, NULL, buf, size, handle);
  if (result == DECODE_ENDGROUP) {
    goto_endmsg(d);
  }
  CHECK_RETURN(result);
  UPB_UNUSED(group);

#define VMCASE(op, code) \
  case op: { code; if (consumes_input(op)) checkpoint(d); break; }
#define PRIMITIVE_OP(type, wt, name, convfunc, ctype) \
  VMCASE(OP_PARSE_ ## type, { \
    ctype val; \
    CHECK_RETURN(decode_ ## wt(d, &val)); \
    upb_sink_put ## name(&d->top->sink, arg, (convfunc)(val)); \
  })

  while(1) {
    d->last = d->pc;
    int32_t instruction = *d->pc++;
    opcode op = getop(instruction);
    uint32_t arg = instruction >> 8;
    int32_t longofs = arg;
    assert(d->ptr != d->residual_end);
#ifdef UPB_DUMP_BYTECODE
    fprintf(stderr, "s_ofs=%d buf_ofs=%d data_rem=%d buf_rem=%d delim_rem=%d "
                    "%x %s (%d)\n",
            (int)offset(d),
            (int)(d->ptr - d->buf),
            (int)(d->data_end - d->ptr),
            (int)(d->end - d->ptr),
            (int)((d->top->end_ofs - d->bufstart_ofs) - (d->ptr - d->buf)),
            (int)(d->pc - 1 - group->bytecode),
            upb_pbdecoder_getopname(op),
            arg);
#endif
    switch (op) {
      // Technically, we are losing data if we see a 32-bit varint that is not
      // properly sign-extended.  We could detect this and error about the data
      // loss, but proto2 does not do this, so we pass.
      PRIMITIVE_OP(INT32,    varint,  int32,  int32_t,      uint64_t)
      PRIMITIVE_OP(INT64,    varint,  int64,  int64_t,      uint64_t)
      PRIMITIVE_OP(UINT32,   varint,  uint32, uint32_t,     uint64_t)
      PRIMITIVE_OP(UINT64,   varint,  uint64, uint64_t,     uint64_t)
      PRIMITIVE_OP(FIXED32,  fixed32, uint32, uint32_t,     uint32_t)
      PRIMITIVE_OP(FIXED64,  fixed64, uint64, uint64_t,     uint64_t)
      PRIMITIVE_OP(SFIXED32, fixed32, int32,  int32_t,      uint32_t)
      PRIMITIVE_OP(SFIXED64, fixed64, int64,  int64_t,      uint64_t)
      PRIMITIVE_OP(BOOL,     varint,  bool,   bool,         uint64_t)
      PRIMITIVE_OP(DOUBLE,   fixed64, double, as_double,    uint64_t)
      PRIMITIVE_OP(FLOAT,    fixed32, float,  as_float,     uint32_t)
      PRIMITIVE_OP(SINT32,   varint,  int32,  upb_zzdec_32, uint64_t)
      PRIMITIVE_OP(SINT64,   varint,  int64,  upb_zzdec_64, uint64_t)

      VMCASE(OP_SETDISPATCH,
        d->top->base = d->pc - 1;
        memcpy(&d->top->dispatch, d->pc, sizeof(void*));
        d->pc += sizeof(void*) / sizeof(uint32_t);
      )
      VMCASE(OP_STARTMSG,
        CHECK_SUSPEND(upb_sink_startmsg(&d->top->sink));
      )
      VMCASE(OP_ENDMSG,
        CHECK_SUSPEND(upb_sink_endmsg(&d->top->sink, d->status));
      )
      VMCASE(OP_STARTSEQ,
        upb_pbdecoder_frame *outer = outer_frame(d);
        CHECK_SUSPEND(upb_sink_startseq(&outer->sink, arg, &d->top->sink));
      )
      VMCASE(OP_ENDSEQ,
        CHECK_SUSPEND(upb_sink_endseq(&d->top->sink, arg));
      )
      VMCASE(OP_STARTSUBMSG,
        upb_pbdecoder_frame *outer = outer_frame(d);
        CHECK_SUSPEND(upb_sink_startsubmsg(&outer->sink, arg, &d->top->sink));
      )
      VMCASE(OP_ENDSUBMSG,
        CHECK_SUSPEND(upb_sink_endsubmsg(&d->top->sink, arg));
      )
      VMCASE(OP_STARTSTR,
        uint32_t len = d->top->end_ofs - offset(d);
        upb_pbdecoder_frame *outer = outer_frame(d);
        CHECK_SUSPEND(upb_sink_startstr(&outer->sink, arg, len, &d->top->sink));
        if (len == 0) {
          d->pc++;  // Skip OP_STRING.
        }
      )
      VMCASE(OP_STRING,
        uint32_t len = curbufleft(d);
        size_t n = upb_sink_putstring(&d->top->sink, arg, d->ptr, len, handle);
        if (n > len) {
          if (n > d->top->end_ofs - offset(d)) {
            seterr(d, "Tried to skip past end of string.");
            return upb_pbdecoder_suspend(d);
          } else {
            int32_t ret = skip(d, n);
            // This shouldn't return DECODE_OK, because n > len.
            assert(ret >= 0);
            return ret;
          }
        }
        advance(d, n);
        if (n < len || d->delim_end == NULL) {
          // We aren't finished with this string yet.
          d->pc--;  // Repeat OP_STRING.
          if (n > 0) checkpoint(d);
          return upb_pbdecoder_suspend(d);
        }
      )
      VMCASE(OP_ENDSTR,
        CHECK_SUSPEND(upb_sink_endstr(&d->top->sink, arg));
      )
      VMCASE(OP_PUSHTAGDELIM,
        CHECK_SUSPEND(pushtagdelim(d, arg));
      )
      VMCASE(OP_SETBIGGROUPNUM,
        d->top->groupnum = *d->pc++;
      )
      VMCASE(OP_POP,
        assert(d->top > d->stack);
        decoder_pop(d);
      )
      VMCASE(OP_PUSHLENDELIM,
        uint32_t len;
        CHECK_RETURN(decode_v32(d, &len));
        CHECK_SUSPEND(decoder_push(d, offset(d) + len));
        set_delim_end(d);
      )
      VMCASE(OP_SETDELIM,
        set_delim_end(d);
      )
      VMCASE(OP_CHECKDELIM,
        // We are guaranteed of this assert because we never allow ourselves to
        // consume bytes beyond data_end, which covers delim_end when non-NULL.
        assert(!(d->delim_end && d->ptr > d->delim_end));
        if (d->ptr == d->delim_end)
          d->pc += longofs;
      )
      VMCASE(OP_CALL,
        d->callstack[d->call_len++] = d->pc;
        d->pc += longofs;
      )
      VMCASE(OP_RET,
        assert(d->call_len > 0);
        d->pc = d->callstack[--d->call_len];
      )
      VMCASE(OP_BRANCH,
        d->pc += longofs;
      )
      VMCASE(OP_TAG1,
        CHECK_SUSPEND(curbufleft(d) > 0);
        uint8_t expected = (arg >> 8) & 0xff;
        if (*d->ptr == expected) {
          advance(d, 1);
        } else {
          int8_t shortofs;
         badtag:
          shortofs = arg;
          if (shortofs == LABEL_DISPATCH) {
            CHECK_RETURN(dispatch(d));
          } else {
            d->pc += shortofs;
            break; // Avoid checkpoint().
          }
        }
      )
      VMCASE(OP_TAG2,
        CHECK_SUSPEND(curbufleft(d) > 0);
        uint16_t expected = (arg >> 8) & 0xffff;
        if (curbufleft(d) >= 2) {
          uint16_t actual;
          memcpy(&actual, d->ptr, 2);
          if (expected == actual) {
            advance(d, 2);
          } else {
            goto badtag;
          }
        } else {
          int32_t result = upb_pbdecoder_checktag_slow(d, expected);
          if (result == DECODE_MISMATCH) goto badtag;
          if (result >= 0) return result;
        }
      )
      VMCASE(OP_TAGN, {
        uint64_t expected;
        memcpy(&expected, d->pc, 8);
        d->pc += 2;
        int32_t result = upb_pbdecoder_checktag_slow(d, expected);
        if (result == DECODE_MISMATCH) goto badtag;
        if (result >= 0) return result;
      })
      VMCASE(OP_DISPATCH, {
        CHECK_RETURN(dispatch(d));
      })
      VMCASE(OP_HALT, {
        return size;
      })
    }
  }
}

void *upb_pbdecoder_startbc(void *closure, const void *pc, size_t size_hint) {
  upb_pbdecoder *d = closure;
  UPB_UNUSED(size_hint);
  d->call_len = 1;
  d->pc = pc;
  return d;
}

void *upb_pbdecoder_startjit(void *closure, const void *hd, size_t size_hint) {
  UPB_UNUSED(hd);
  UPB_UNUSED(size_hint);
  upb_pbdecoder *d = closure;
  d->call_len = 0;
  return d;
}

bool upb_pbdecoder_end(void *closure, const void *handler_data) {
  upb_pbdecoder *d = closure;
  const upb_pbdecodermethod *method = handler_data;

  if (d->residual_end > d->residual) {
    seterr(d, "Unexpected EOF");
    return false;
  }

  if (d->top->end_ofs != UINT64_MAX) {
    seterr(d, "Unexpected EOF inside delimited string");
    return false;
  }

  // Message ends here.
  uint64_t end = offset(d);
  d->top->end_ofs = end;

  char dummy;
#ifdef UPB_USE_JIT_X64
  const mgroup *group = (const mgroup*)method->group;
  if (group->jit_code) {
    if (d->top != d->stack)
      d->stack->end_ofs = 0;
    group->jit_code(closure, method->code_base.ptr, &dummy, 0, NULL);
  } else {
#endif
    d->stack->end_ofs = end;
    const uint32_t *p = d->pc;
    // Check the previous bytecode, but guard against beginning.
    if (p != method->code_base.ptr) p--;
    if (getop(*p) == OP_CHECKDELIM) {
      // Rewind from OP_TAG* to OP_CHECKDELIM.
      assert(getop(*d->pc) == OP_TAG1 ||
             getop(*d->pc) == OP_TAG2 ||
             getop(*d->pc) == OP_TAGN ||
             getop(*d->pc == OP_DISPATCH));
      d->pc = p;
    }
    upb_pbdecoder_decode(closure, handler_data, &dummy, 0, NULL);
#ifdef UPB_USE_JIT_X64
  }
#endif

  if (d->call_len != 0) {
    seterr(d, "Unexpected EOF");
    return false;
  }

  return true;
}

void upb_pbdecoder_init(upb_pbdecoder *d, const upb_pbdecodermethod *m,
                        upb_status *s) {
  d->limit = &d->stack[UPB_DECODER_MAX_NESTING];
  upb_bytessink_reset(&d->input_, &m->input_handler_, d);
  d->method_ = m;
  d->callstack[0] = &halt;
  d->status = s;
  upb_pbdecoder_reset(d);
}

void upb_pbdecoder_reset(upb_pbdecoder *d) {
  d->top = d->stack;
  d->top->end_ofs = UINT64_MAX;
  d->top->groupnum = 0;
  d->bufstart_ofs = 0;
  d->ptr = d->residual;
  d->buf = d->residual;
  d->end = d->residual;
  d->residual_end = d->residual;
  d->call_len = 1;
}

uint64_t upb_pbdecoder_bytesparsed(const upb_pbdecoder *d) {
  return offset(d);
}

// Not currently required, but to support outgrowing the static stack we need
// this.
void upb_pbdecoder_uninit(upb_pbdecoder *d) {
  UPB_UNUSED(d);
}

const upb_pbdecodermethod *upb_pbdecoder_method(const upb_pbdecoder *d) {
  return d->method_;
}

bool upb_pbdecoder_resetoutput(upb_pbdecoder *d, upb_sink* sink) {
  // TODO(haberman): do we need to test whether the decoder is already on the
  // stack (like calling this from within a callback)?  Should we support
  // rebinding the output at all?
  assert(sink);
  if (d->method_->dest_handlers_) {
    if (sink->handlers != d->method_->dest_handlers_)
      return false;
  }
  upb_sink_reset(&d->top->sink, sink->handlers, sink->closure);
  return true;
}

upb_bytessink *upb_pbdecoder_input(upb_pbdecoder *d) {
  return &d->input_;
}
