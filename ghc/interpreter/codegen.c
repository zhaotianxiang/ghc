
/* --------------------------------------------------------------------------
 * Code generator
 *
 * The Hugs 98 system is Copyright (c) Mark P Jones, Alastair Reid, the
 * Yale Haskell Group, and the Oregon Graduate Institute of Science and
 * Technology, 1994-1999, All rights reserved.  It is distributed as
 * free software under the license in the file "License", which is
 * included in the distribution.
 *
 * $RCSfile: codegen.c,v $
 * $Revision: 1.23 $
 * $Date: 2000/04/27 16:35:29 $
 * ------------------------------------------------------------------------*/

#include "hugsbasictypes.h"
#include "storage.h"
#include "connect.h"
#include "errors.h"

#include "Rts.h"       /* to make StgPtr visible in Assembler.h */
#include "Assembler.h"
#include "RtsFlags.h"

/*#define DEBUG_CODEGEN*/

/*  (JRS, 27 Apr 2000):

A total rewrite of the BCO assembler/linker, and rationalisation of
the code management and code generation phases of Hugs.

Problems with the old linker:

* Didn't have a clean way to insert a pointer to GHC code into a BCO.
  This meant CAF GC didn't work properly in combined mode.

* Leaked memory.  Each BCO, caf and constructor generated by Hugs had
  a corresponding malloc'd record used in its construction.  These
  records existed forever.  Pointers from the Hugs symbol tables into
  the runtime heap always went via these intermediates, for no apparent
  reason.

* A global variable holding a list of top-level stg trees was used
  during code generation.  It was hard to associate trees in this
  list with entries in the name/tycon tables.  Just too many
  mechanisms.

The New World Order is as follows:

* The global code list (stgGlobals) is gone.

* Each name in the name table has a .closure field.  This points
  to the top-level code for that name.  Before bytecode generation
  this points to a STG tree.  During bytecode generation but before
  bytecode linking it is a MPtr pointing to a malloc'd intermediate
  structure (an AsmObject).  After linking, it is a real live pointer
  into the execution heap (CPtr) which is treated as a root during GC.

  Because tuples do not have name table entries, tycons which are
  tuples also have a .closure field, which is treated identically
  to those of name table entries.

* Each module has a code list -- a list of names and tuples.  If you
  are a name or tuple and you have something (code, CAF or Con) which
  needs to wind up in the execution heap, you MUST be on your module's
  code list.  Otherwise you won't get code generated.

* Lambda lifting generates new name table entries, which of course
  also wind up on the code list.

* The initial phase of code generation for a module m traverses m's
  code list.  The stg trees referenced in the .closure fields are
  code generated, creating AsmObject (AsmBCO, AsmCAF, AsmCon) in
  mallocville.  The .closure fields then point to these AsmObjects.
  Since AsmObjects can be mutually recursive, they can contain
  references to:
     * Other AsmObjects            Asm_RefObject
     * Existing closures           Asm_RefNoOp
     * name/tycon table entries    Asm_RefHugs
  AsmObjects can also contain BCO insns and non-ptr words.

* A second copy-and-link phase copies the AsmObjects into the
  execution heap, resolves the Asm_Ref* items, and frees up
  the malloc'd entities.

* Minor cleanups in compile-time storage.  There are now 3 kinds of
  address-y things available:
     CPtr/mkCPtr/cptrOf    -- ptrs to Closures, probably in exec heap
                              ie anything which the exec GC knows about
     MPtr/mkMPtr/mptrOf    -- ptrs to mallocville, which the exec GC
                              knows nothing about
     Addr/mkAddr/addrOf    -- literal addresses (like literal ints)

* Many hacky cases removed from codegen.c.  Referencing code or
  data during code generation is a lot simpler, since an entity
  is either:
      a CPtr, in which case use it as is
      a MPtr -- stuff it into the AsmObject and the linker will fix it
      a name or tycon
             -- ditto

* I've checked, using Purify that, at least in standalone mode,
  no longer leaks mallocd memory.  Prior to this it would leak at
  the rate of about 300k per Prelude.

Still to do:

* Reinstate peephole optimisation for BCOs.

* Nuke magic number headers in AsmObjects, used for debugging.

* Profile and accelerate.  Code generation is slower because linking
  is slower.  Evaluation GC is slower because markHugsObjects has
  sloweed down.

* Make setCurrentModule ignore name table entries created by the
  lambda-lifter.

* Zap various #if 0 in codegen.c/Assembler.c.

* Zap CRUDE_PROFILING.
*/


/* --------------------------------------------------------------------------
 * Local function prototypes:
 * ------------------------------------------------------------------------*/

#define getPos(v)     intOf(stgVarInfo(v))
#define setPos(v,sp)  stgVarInfo(v) = mkInt(sp)
#define getObj(v)     mptrOf(stgVarInfo(v))
#define setObj(v,obj) stgVarInfo(v) = mkMPtr(obj)

#define repOf(x)      charOf(stgVarRep(x))

static void      cgBind       ( AsmBCO bco, StgVar v );
static Void      pushAtom     ( AsmBCO bco, StgAtom atom );
static Void      alloc        ( AsmBCO bco, StgRhs rhs );
static Void      build        ( AsmBCO bco, StgRhs rhs );
static Void      cgExpr       ( AsmBCO bco, AsmSp root, StgExpr e );
             
static AsmBCO    cgAlts       ( AsmSp root, AsmSp sp, List alts );
static void      testPrimPats ( AsmBCO bco, AsmSp root, List pats, StgExpr e );
static AsmBCO    cgLambda     ( StgExpr e );
static AsmBCO    cgRhs        ( StgRhs rhs );
static void      beginTop     ( StgVar v );
static AsmObject endTop       ( StgVar v );

static StgVar currentTop;

/* --------------------------------------------------------------------------
 * 
 * ------------------------------------------------------------------------*/

static void* /* StgClosure*/ cptrFromName ( Name n )
{
   char  buf[1000];
   void* p;
   Module m = name(n).mod;
   Text  mt = module(m).text;
   sprintf(buf, MAYBE_LEADING_UNDERSCORE_STR("%s_%s_closure"), 
                textToStr(mt), 
                textToStr( enZcodeThenFindText ( 
                   textToStr (name(n).text) ) ) );
   p = lookupOTabName ( m, buf );
   if (!p) {
      ERRMSG(0) "Can't find object symbol %s", buf
      EEND;
   }
   return p;
}

char* lookupHugsName( void* closure )
{
    extern Name nameHw;
    Name nm;
    for( nm = NAME_BASE_ADDR; 
         nm < NAME_BASE_ADDR+tabNameSz; ++nm ) 
       if (tabName[nm-NAME_BASE_ADDR].inUse) {
           Cell cl = name(nm).closure;
           if (isCPtr(cl) && cptrOf(cl) == closure)
               return textToStr(name(nm).text);
    }
    return NULL;
}

static void cgBindRep( AsmBCO bco, StgVar v, AsmRep rep )
{
    setPos(v,asmBind(bco,rep));
}

static void cgBind( AsmBCO bco, StgVar v )
{
    cgBindRep(bco,v,repOf(v));
}

static void cgAddPtrToObject ( AsmObject obj, Cell ptrish )
{
   switch (whatIs(ptrish)) {
      case CPTRCELL:
         asmAddRefNoOp ( obj, (StgPtr)cptrOf(ptrish) ); break;
      case MPTRCELL:
         asmAddRefObject ( obj, mptrOf(ptrish) ); break;
      default:
         internal("cgAddPtrToObject");
   }
}

#if 0
static void cgPushRef ( AsmBCO bco, Cell c )
{
   switch (whatIs(c)) {
      case CPTRCELL:
         asmPushRefNoOp(bco,(StgPtr)cptrOf(c)); break;
      case PTRCELL:
         asmPushRefObject(bco,ptrOf(c)); break;
      case NAME:
      case TUPLE:
         asmPushRefHugs(bco,c); break;
      default:
         internal("cgPushRef");
   }
}
#endif

/* Get a pointer to atom e onto the stack. */
static Void pushAtom ( AsmBCO bco, StgAtom e )
{
    Cell info;
    Cell cl;
#   if 0
    printf ( "pushAtom: %d  ", e ); fflush(stdout);
    print(e,10);printf("\n");
#   endif
    switch (whatIs(e)) {
       case STGVAR:
           info = stgVarInfo(e);
           if (isInt(info)) {
              asmVar(bco,intOf(info),repOf(e));
           }
           else
           if (isCPtr(info)) { 
              asmPushRefNoOp(bco,cptrOf(info));
           }
           else
           if (isMPtr(info)) { 
              asmPushRefObject(bco,mptrOf(info));
           }
           else {
              internal("pushAtom: STGVAR");
           }
           break;
       case NAME:
       case TUPLE:
            cl = getNameOrTupleClosure(e);
            if (isStgVar(cl)) {
               /* a stg tree which hasn't yet been translated */
               asmPushRefHugs(bco,e);
            }
            else
            if (isCPtr(cl)) {
               /* a pointer to something in the heap */
               asmPushRefNoOp(bco,(StgPtr)cptrOf(cl));
            } 
            else
            if (isMPtr(cl)) {
               /* a pointer to an AsmBCO/AsmCAF/AsmCon object */
               asmPushRefObject(bco,mptrOf(cl));
            }
            else {
               StgClosure* addr; 
               ASSERT(isNull(cl));
               addr = cptrFromName(e);
#              if DEBUG_CODEGEN
               fprintf ( stderr, "nativeAtom: name %s\n", 
                                 nameFromOPtr(addr) );
#              endif
	       asmPushRefNoOp(bco,(StgPtr)addr);
            }
            break;
       case CHARCELL: 
            asmConstChar(bco,charOf(e));
            break;
       case INTCELL: 
            asmConstInt(bco,intOf(e));
            break;
       case ADDRCELL: 
            asmConstAddr(bco,addrOf(e));
            break;
       case BIGCELL:
            asmConstInteger(bco,bignumToString(e)); 
            break;
       case FLOATCELL: 
            asmConstDouble(bco,floatOf(e));
            break;
       case STRCELL: 
#           if USE_ADDR_FOR_STRINGS
            asmConstAddr(bco,textToStr(textOf(e)));
#           else
            asmClosure(bco,asmStringObj(textToStr(textOf(e))));
#           endif
            break;
       case CPTRCELL:
            asmPushRefNoOp(bco,cptrOf(e));
            break;
       case MPTRCELL: 
            asmPushRefObject(bco,mptrOf(e));
            break;
       default: 
            fprintf(stderr,"\nYoiks1: "); printExp(stderr,e);
            internal("pushAtom");
    }
}

static AsmBCO cgAlts( AsmSp root, AsmSp sp, List alts )
{
#ifdef CRUDE_PROFILING
    AsmBCO bco = asmBeginContinuation(sp, currentTop + 1000000000);
#else
    AsmBCO bco = asmBeginContinuation(sp, alts);
#endif
    Bool omit_test
       = length(alts) == 2 &&
         isDefaultAlt(hd(tl(alts))) &&
         !isDefaultAlt(hd(alts));
    if (omit_test) {
       /* refine the condition */              
       Name con;
       Tycon t;
       omit_test = FALSE;
       con = stgCaseAltCon(hd(alts));

       /* special case: dictionary constructors */
       if (isName(con) && strncmp(":D",textToStr(name(con).text),2)==0) {
          omit_test = TRUE;
          goto xyzzy;
       }
       /* special case: Tuples */
       if (isTuple(con) || (isName(con) && con==nameUnit)) {
          omit_test = TRUE;
          goto xyzzy;
       }          

       t = name(con).parent;
       if (tycon(t).what == DATATYPE) {
          if (length(tycon(t).defn) == 1) omit_test = TRUE;
       }
    }

    xyzzy:

    for(; nonNull(alts); alts=tl(alts)) {
        StgCaseAlt alt  = hd(alts);
        if (isDefaultAlt(alt)) {
            cgBind(bco,stgDefaultVar(alt));
            cgExpr(bco,root,stgDefaultBody(alt));
            asmEndContinuation(bco);
            return bco; /* ignore any further alternatives */
        } else {
            StgDiscr con   = stgCaseAltCon(alt);
            List     vs    = stgCaseAltVars(alt);
            AsmSp    begin = asmBeginAlt(bco);
            AsmPc    fix;
            if (omit_test) fix=-1; else fix = asmTest(bco,stgDiscrTag(con)); 

	    asmBind(bco,PTR_REP); /* Adjust simulated sp to acct for return value */
            if (isBoxingCon(con)) {
                setPos(hd(vs),asmUnbox(bco,boxingConRep(con)));
            } else {
                asmBeginUnpack(bco);
                map1Proc(cgBind,bco,reverse(vs));
                asmEndUnpack(bco);
            }
            cgExpr(bco,root,stgCaseAltBody(alt));
            asmEndAlt(bco,begin);
            if (fix != -1) asmFixBranch(bco,fix);
        }
    }
    /* if we got this far and didn't match, panic! */
    asmPanic(bco);
    asmEndContinuation(bco);
    return bco;
}

static void testPrimPats( AsmBCO bco, AsmSp root, List pats, StgExpr e )
{
    if (isNull(pats)) {
        cgExpr(bco,root,e);
    } else {
        StgVar pat = hd(pats);
        if (isInt(stgVarBody(pat))) {
            /* asmTestInt leaves stack unchanged - so no need to adjust it */
            AsmPc tst = asmTestInt(bco,getPos(pat),intOf(stgVarBody(pat)));
            assert(repOf(pat) == INT_REP);
            testPrimPats(bco,root,tl(pats),e);
            asmFixBranch(bco,tst);
        } else {
            testPrimPats(bco,root,tl(pats),e);
        }
    }
}


static AsmBCO cgLambda( StgExpr e )
{
    AsmBCO bco = asmBeginBCO(e);

    AsmSp root = asmBeginArgCheck(bco);
    map1Proc(cgBind,bco,reverse(stgLambdaArgs(e)));
    asmEndArgCheck(bco,root);

    /* ppStgExpr(e); */
    cgExpr(bco,root,stgLambdaBody(e));

    asmEndBCO(bco);
    return bco;
}

static AsmBCO cgRhs( StgRhs rhs )
{
    AsmBCO bco = asmBeginBCO(rhs );

    AsmSp root = asmBeginArgCheck(bco);
    asmEndArgCheck(bco,root);

    /* ppStgExpr(rhs); */
    cgExpr(bco,root,rhs);

    asmEndBCO(bco);
    return bco;
}


static Void cgExpr( AsmBCO bco, AsmSp root, StgExpr e )
{
#if 0
    printf("cgExpr:");ppStgExpr(e);printf("\n");
#endif
    switch (whatIs(e)) {
    case LETREC:
        {
            List binds = stgLetBinds(e);
            map1Proc(alloc,bco,binds);
            map1Proc(build,bco,binds);
            cgExpr(bco,root,stgLetBody(e));
            break;
        }
    case LAMBDA:
        {
            AsmSp begin = asmBeginEnter(bco);
            asmPushRefObject(bco,cgLambda(e));
            asmEndEnter(bco,begin,root);
            break;
        }
    case CASE:
        {
            List  alts     = stgCaseAlts(e);
            AsmSp sp       = asmBeginCase(bco);
            AsmSp caseroot = asmContinuation(bco,cgAlts(root,sp,alts));
            cgExpr(bco,caseroot,stgCaseScrut(e));
            asmEndCase(bco);
            break;
        }
    case PRIMCASE:
        {
            StgExpr scrut = stgPrimCaseScrut(e);
            List alts = stgPrimCaseAlts(e);
            if (whatIs(scrut) == STGPRIM) {  /* this is an optimisation */

                /* No need to use return address or to Slide */
                AsmSp beginPrim = asmBeginPrim(bco);
                map1Proc(pushAtom,bco,reverse(stgPrimArgs(scrut)));
                asmEndPrim(bco,(AsmPrim*)name(stgPrimOp(scrut)).primop,beginPrim);

                for(; nonNull(alts); alts=tl(alts)) {
                    StgPrimAlt alt = hd(alts);
                    List    pats = stgPrimAltVars(alt);
                    StgExpr body = stgPrimAltBody(alt);
                    AsmSp altBegin = asmBeginAlt(bco);
                    map1Proc(cgBind,bco,reverse(pats));
                    testPrimPats(bco,root,pats,body);
                    asmEndAlt(bco,altBegin);
                }
                /* if we got this far and didn't match, panic! */
                asmPanic(bco);
                
            } else if (whatIs(scrut) == STGVAR) { /* another optimisation */

                /* No need to use return address or to Slide */

                /* only part different from primop code... todo */
                AsmSp beginCase = asmBeginCase(bco);
                pushAtom /*pushVar*/ (bco,scrut);
                asmEndAlt(bco,beginCase); /* hack, hack -  */

                for(; nonNull(alts); alts=tl(alts)) {
                    StgPrimAlt alt = hd(alts);
                    List    pats = stgPrimAltVars(alt);
                    StgExpr body = stgPrimAltBody(alt);
                    AsmSp altBegin = asmBeginAlt(bco);
                    map1Proc(cgBind,bco,pats);
                    testPrimPats(bco,root,pats,body);
                    asmEndAlt(bco,altBegin);
                }
                /* if we got this far and didn't match, panic! */
                asmPanic(bco);
                                
            } else {
                /* ToDo: implement this code...  */
                assert(0);
                /* asmPushRet(bco,delayPrimAlt( stgPrimCaseVars(e), 
                                                stgPrimCaseBody(e))); */
                /* cgExpr( bco,root,scrut ); */
            }
            break;
        }
    case STGAPP: /* Tail call */
        {
            AsmSp env = asmBeginEnter(bco);
            map1Proc(pushAtom,bco,reverse(stgAppArgs(e)));
            pushAtom(bco,stgAppFun(e));
            asmEndEnter(bco,env,root);
            break;
        }
    case TUPLE:
    case NAME: /* Tail call (with no args) */
        {
            AsmSp env = asmBeginEnter(bco);
            /* JRS 000112: next line used to be: pushVar(bco,name(e).stgVar); */
            pushAtom(bco,e);
            asmEndEnter(bco,env,root);
            break;
        }
    case STGVAR: /* Tail call (with no args), plus unboxed return */
            switch (repOf(e)) {
            case PTR_REP:
            case ALPHA_REP:
            case BETA_REP:
                {
                    AsmSp env = asmBeginEnter(bco);
                    pushAtom /*pushVar*/ (bco,e);
                    asmEndEnter(bco,env,root);
                    break;
                }
            case INT_REP:
                    assert(0);
                    /* cgTailCall(bco,singleton(e)); */
                    /* asmReturnInt(bco); */
                    break;
            default:
                    internal("cgExpr StgVar");
            }
            break;
    case STGPRIM: /* Tail call again */
        {
            AsmSp beginPrim = asmBeginPrim(bco);
            map1Proc(pushAtom,bco,reverse(stgPrimArgs(e)));
            asmEndPrim(bco,(AsmPrim*)name(e).primop,beginPrim);
            /* map1Proc(cgBind,bco,rs_vars); */
            assert(0); /* asmReturn_retty(); */
            break;
        }
    default:
            fprintf(stderr,"\nYoiks2: "); printExp(stderr,e);
            internal("cgExpr");
    }
}

/* allocate space for top level variable
 * any change requires a corresponding change in 'build'.
 */
static Void alloc( AsmBCO bco, StgVar v )
{
    StgRhs rhs = stgVarBody(v);
    assert(isStgVar(v));
#if 0
    printf("alloc: ");ppStgExpr(v);
#endif
    switch (whatIs(rhs)) {
    case STGCON:
        {
            StgDiscr con  = stgConCon(rhs);
            List     args = stgConArgs(rhs);
            if (isBoxingCon(con)) {
                pushAtom(bco,hd(args));
                setPos(v,asmBox(bco,boxingConRep(con)));
            } else {
                setPos(v,asmAllocCONSTR(bco,stgConInfo(con)));
            }
            break;
        }
    case STGAPP: {
            Int  totSizeW = 0;
            List bs       = stgAppArgs(rhs);
            for (; nonNull(bs); bs=tl(bs)) {
               if (isName(hd(bs))) {
                  totSizeW += 1;
               } else {
                  ASSERT(whatIs(hd(bs))==STGVAR);
                  totSizeW += asmRepSizeW( charOf(stgVarRep(hd(bs))) );
               }
            }
            setPos(v,asmAllocAP(bco,totSizeW));
            break;
         }
    case LAMBDA: /* optimisation */
            setObj(v,cgLambda(rhs));
            break;
    default: 
            setPos(v,asmAllocAP(bco,0));
            break;
    }
}

static Void build( AsmBCO bco, StgVar v )
{
    StgRhs rhs = stgVarBody(v);
    assert(isStgVar(v));
    //ppStg(v);
    switch (whatIs(rhs)) {
    case STGCON:
        {
            StgDiscr con  = stgConCon(rhs);
            List     args = stgConArgs(rhs);
            if (isBoxingCon(con)) {
                doNothing();  /* already done in alloc */
            } else {
                AsmSp start = asmBeginPack(bco);
                map1Proc(pushAtom,bco,reverse(args));
                asmEndPack(bco,getPos(v),start,stgConInfo(con));
            }
            return;
        }
    case STGAPP: 
        {
            Bool   itsaPAP;
            StgVar fun  = stgAppFun(rhs);
            List   args = stgAppArgs(rhs);

            if (isName(fun)) {
               itsaPAP = name(fun).arity > length(args);
            } else
            if (isStgVar(fun)) {
               itsaPAP = FALSE;
               if (nonNull(stgVarBody(fun))
                   && whatIs(stgVarBody(fun)) == LAMBDA 
                   && length(stgLambdaArgs(stgVarBody(fun))) > length(args)
                  )
                  itsaPAP = TRUE;
            }
            else
               internal("build: STGAPP");
#if 0
Looks like a hack to me.
            if (isName(fun)) {
                if (nonNull(name(fun).closure))
                   fun = name(fun).closure; else
                   fun = cptrFromName(fun);
            }

            if (isCPtr(fun)) {
               assert(isName(fun0));
               itsaPAP = name(fun0).arity > length(args);
#              if DEBUG_CODEGEN
               fprintf ( stderr, "nativeCall: name %s, arity %d, args %d\n",
                         nameFromOPtr(cptrOf(fun)), name(fun0).arity,
                         length(args) );
#              endif
            } else {
               itsaPAP = FALSE;
               if (nonNull(stgVarBody(fun))
                   && whatIs(stgVarBody(fun)) == LAMBDA 
                   && length(stgLambdaArgs(stgVarBody(fun))) > length(args)
                  )
                  itsaPAP = TRUE;
            }
#endif

            if (itsaPAP) {
                AsmSp  start = asmBeginMkPAP(bco);
                map1Proc(pushAtom,bco,reverse(args));
                pushAtom(bco,fun);
                asmEndMkPAP(bco,getPos(v),start); /* optimisation */
            } else {
                AsmSp  start = asmBeginMkAP(bco);
                map1Proc(pushAtom,bco,reverse(args));
                pushAtom(bco,fun);
                asmEndMkAP(bco,getPos(v),start);
            }
            return;
        }
    case LAMBDA: /* optimisation */
            doNothing(); /* already pushed in alloc */
            break;

    /* These two cases look almost identical to the default but they're really
     * special cases of STGAPP.  The essential thing here is that we can't call
     * cgRhs(rhs) because that expects the rhs to have no free variables when, 
     * in fact, the rhs is _always_ a free variable.
     *
     * ToDo: a simple optimiser would eliminate all examples
     * of this except "let x = x in ..."
     */
    case NAME:
    case STGVAR:
        {
            AsmSp  start = asmBeginMkAP(bco);
            pushAtom(bco,rhs);
            asmEndMkAP(bco,getPos(v),start);
        }
        return;
    default:
        {
            AsmSp start = asmBeginMkAP(bco);   /* make it updateable! */
            asmPushRefObject(bco,cgRhs(rhs));
            asmEndMkAP(bco,getPos(v),start);
            return;
        }
    }
}

/* --------------------------------------------------------------------------
 * Top level variables
 *
 * ToDo: these should be handled by allocating a dynamic unentered CAF
 * for each top level variable - this should be simpler!
 * ------------------------------------------------------------------------*/

/* allocate AsmObject for top level variables
 * any change requires a corresponding change in endTop
 */
static void beginTop( StgVar v )
{
    StgRhs rhs;
    assert(isStgVar(v));
    currentTop = v;
    rhs = stgVarBody(v);
    switch (whatIs(rhs)) {
       case STGCON:
          setObj(v,asmBeginCon(stgConInfo(stgConCon(rhs))));
          break;
       case LAMBDA:
#         ifdef CRUDE_PROFILING
          setObj(v,asmBeginBCO(currentTop));
#         else
          setObj(v,asmBeginBCO(rhs));
#         endif
          break;
       default:
          setObj(v,asmBeginCAF());
          break;
    }
}

static AsmObject endTop( StgVar v )
{
    StgRhs rhs = stgVarBody(v);
    currentTop = v;
    switch (whatIs(rhs)) {
       case STGCON: {
          List as = stgConArgs(rhs);
          AsmCon con = (AsmCon)getObj(v);
          for ( ; nonNull(as); as=tl(as)) {
             StgAtom a = hd(as);
             switch (whatIs(a)) {
                case STGVAR: 
                   /* should be a delayed combinator! */
                   asmAddRefObject(con,(AsmObject)getObj(a));
                   break;
                case NAME: {
                   StgVar var = name(a).closure;
                   cgAddPtrToObject(con,var);
                   break;
                }
#               if !USE_ADDR_FOR_STRINGS
                case STRCELL:
                   asmAddPtr(con,asmStringObj(textToStr(textOf(a))));
                   break;
#               endif
                default: 
                   /* asmAddPtr(con,??); */
                   assert(0);
                   break;
             }
          }
          asmEndCon(con);
          return con;
       }
       case LAMBDA: { /* optimisation */
          /* ToDo: merge this code with cgLambda */
          AsmBCO bco = (AsmBCO)getObj(v);
          AsmSp root = asmBeginArgCheck(bco);
          map1Proc(cgBind,bco,reverse(stgLambdaArgs(rhs)));
          asmEndArgCheck(bco,root);
            
          cgExpr(bco,root,stgLambdaBody(rhs));
         
          asmEndBCO(bco);
          return bco;
       }
       default: {  /* updateable caf */
          AsmCAF caf = (AsmCAF)getObj(v);
          asmAddRefObject ( caf, cgRhs(rhs) );
          asmEndCAF(caf);
          return caf;
       }
    }
}


/* --------------------------------------------------------------------------
 * The external entry points for the code generator.
 * ------------------------------------------------------------------------*/

Void cgModule ( Module mod )
{
    List cl;
    Cell c;
    int i;

    /* Lambda-lift, by traversing the code list of this module.  
       This creates more name-table entries, which are duly added
       to the module's code list.
    */
    liftModule ( mod );

    /* Initialise the BCO linker subsystem. */
    asmInitialise();

    /* Generate BCOs, CAFs and Constructors into mallocville.  
       At this point, the .closure values of the names/tycons on
       the codelist contain StgVars, ie trees.  The call to beginTop
       converts them to MPtrs to AsmObjects.
    */
    for (cl=module(mod).codeList; nonNull(cl); cl=tl(cl)) {
       c = getNameOrTupleClosure(hd(cl));
       if (isCPtr(c)) continue;
#      if 0
       if (isName(hd(cl))) {
          printStg( stdout, name(hd(cl)).closure ); printf( "\n\n"); 
       }
#      endif
       beginTop ( c );
    }

    for (cl=module(mod).codeList; nonNull(cl); cl=tl(cl)) {
       c = getNameOrTupleClosure(hd(cl));
       if (isCPtr(c)) continue;
#      if 0
       if (isName(hd(cl))) {
          printStg( stdout, name(hd(cl)).closure ); printf( "\n\n"); 
       }
#      endif
       setNameOrTupleClosure ( hd(cl), mkMPtr(endTop(c)) );
    }

    //fprintf ( stderr, "\nstarting sanity check\n" );
    for (cl = module(mod).codeList; nonNull(cl); cl=tl(cl)) {
       Cell c = hd(cl);
       ASSERT(isName(c) || isTuple(c));
       c = getNameOrTupleClosure(c);
       ASSERT(isMPtr(c) || isCPtr(c));
    }
    //fprintf ( stderr, "completed sanity check\n" );


    /* Figure out how big each object will be in the evaluator's heap,
       and allocate space to put each in, but don't copy yet.  Record
       the heap address in the object.  Assumes that GC doesn't happen;
       reasonable since we use allocate().
    */
    asmAllocateHeapSpace();

    /* Update name/tycon table closure entries with these new heap addrs. */
    for (cl = module(mod).codeList; nonNull(cl); cl=tl(cl)) {
       c = getNameOrTupleClosure(hd(cl));
       if (isMPtr(c))
          setNameOrTupleClosureCPtr ( 
             hd(cl), asmGetClosureOfObject(mptrOf(c)) );
    }

    /* Copy out of mallocville into the heap, resolving references on
       the way.
    */
    asmCopyAndLink();

    /* Free up the malloc'd memory. */
    asmShutdown();
}


/* --------------------------------------------------------------------------
 * Code Generator control:
 * ------------------------------------------------------------------------*/

Void codegen(what)
Int what; {
    switch (what) {
       case PREPREL:
       case RESET: 
       case MARK: 
       case POSTPREL:
          break;
    }
    liftControl(what);
}

/*-------------------------------------------------------------------------*/
