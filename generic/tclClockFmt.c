/*
 * tclClockFmt.c --
 *
 *	Contains the date format (and scan) routines. This code is back-ported
 *	from the time and date facilities of tclSE engine, by Serg G. Brester.
 *
 * Copyright (c) 2015 by Sergey G. Brester aka sebres. All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tclInt.h"
#include "tclStrIdxTree.h"
#include "tclDate.h"

/*
 * Miscellaneous forward declarations and functions used within this file
 */

static void
ClockFmtObj_DupInternalRep(Tcl_Obj *srcPtr, Tcl_Obj *copyPtr);
static void
ClockFmtObj_FreeInternalRep(Tcl_Obj *objPtr);
static int
ClockFmtObj_SetFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr);
static void
ClockFmtObj_UpdateString(Tcl_Obj *objPtr);


TCL_DECLARE_MUTEX(ClockFmtMutex); /* Serializes access to common format list. */

static void ClockFmtScnStorageDelete(ClockFmtScnStorage *fss);

#ifndef TCL_CLOCK_FULL_COMPAT
#define TCL_CLOCK_FULL_COMPAT 1
#endif

/*
 * Clock scan and format facilities.
 */

/*
 *----------------------------------------------------------------------
 *
 * _str2int -- , _str2wideInt --
 *
 *	Fast inline-convertion of string to signed int or wide int by given
 *	start/end.
 *
 *	The given string should contain numbers chars only (because already
 *	pre-validated within parsing routines)
 *
 * Results:
 *	Returns a standard Tcl result.
 *	TCL_OK - by successful conversion, TCL_ERROR by (wide) int overflow
 *
 *----------------------------------------------------------------------
 */

static inline void
_str2int_no(
    int	       *out,
    register
    const char *p,
    const char *e,
    int sign)
{
    /* assert(e <= p+10); */
    register int val = 0;
    /* overflow impossible for 10 digits ("9..9"), so no needs to check at all */
    while (p < e) {			/* never overflows */
	val = val * 10 + (*p++ - '0');
    }
    if (sign < 0) { val = -val; }
    *out = val;
}

static inline void
_str2wideInt_no(
    Tcl_WideInt *out,
    register
    const char	*p,
    const char	*e,
    int sign)
{
    /* assert(e <= p+18); */
    register Tcl_WideInt val = 0;
    /* overflow impossible for 18 digits ("9..9"), so no needs to check at all */
    while (p < e) {			/* never overflows */
	val = val * 10 + (*p++ - '0');
    }
    if (sign < 0) { val = -val; }
    *out = val;
}

/* int & Tcl_WideInt overflows may happens here (expected case) */
#if (defined(__GNUC__) || defined(__GNUG__)) && !defined(__clang__)
# pragma GCC optimize("no-trapv")
#endif

static inline int
_str2int(
    int	       *out,
    register
    const char *p,
    const char *e,
    int sign)
{
    register int val = 0;
    /* overflow impossible for 10 digits ("9..9"), so no needs to check before */
    const char *eNO = p+10;
    if (eNO > e) {
	eNO = e;
    }
    while (p < eNO) {				/* never overflows */
	val = val * 10 + (*p++ - '0');
    }
    if (sign >= 0) {
	while (p < e) {				/* check for overflow */
	    int prev = val;
	    val = val * 10 + (*p++ - '0');
	    if (val / 10 < prev) {
		return TCL_ERROR;
	    }
	}
    } else {
	val = -val;
	while (p < e) {				/* check for overflow */
	    int prev = val;
	    val = val * 10 - (*p++ - '0');
	    if (val / 10 > prev) {
		return TCL_ERROR;
	    }
	}
    }
    *out = val;
    return TCL_OK;
}

static inline int
_str2wideInt(
    Tcl_WideInt *out,
    register
    const char	*p,
    const char	*e,
    int sign)
{
    register Tcl_WideInt val = 0;
    /* overflow impossible for 18 digits ("9..9"), so no needs to check before */
    const char *eNO = p+18;
    if (eNO > e) {
	eNO = e;
    }
    while (p < eNO) {				/* never overflows */
	val = val * 10 + (*p++ - '0');
    }
    if (sign >= 0) {
	while (p < e) {				/* check for overflow */
	    Tcl_WideInt prev = val;
	    val = val * 10 + (*p++ - '0');
	    if (val / 10 < prev) {
		return TCL_ERROR;
	    }
	}
    } else {
	val = -val;
	while (p < e) {				/* check for overflow */
	    Tcl_WideInt prev = val;
	    val = val * 10 - (*p++ - '0');
	    if (val / 10 > prev) {
		return TCL_ERROR;
	    }
	}
    }
    *out = val;
    return TCL_OK;
}

int
TclAtoWIe(
    Tcl_WideInt *out,
    const char	*p,
    const char	*e,
    int sign)
{
    return _str2wideInt(out, p, e, sign);
}

#if (defined(__GNUC__) || defined(__GNUG__)) && !defined(__clang__)
# pragma GCC reset_options
#endif

/*
 *----------------------------------------------------------------------
 *
 * _itoaw -- , _witoaw --
 *
 *	Fast inline-convertion of signed int or wide int to string, using
 *	given padding with specified padchar and width (or without padding).
 *
 *	This is a very fast replacement for sprintf("%02d").
 *
 * Results:
 *	Returns position in buffer after end of conversion result.
 *
 *----------------------------------------------------------------------
 */

static inline char *
_itoaw(
    char *buf,
    register int val,
    char  padchar,
    unsigned short int width)
{
    register char *p;
    static int wrange[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

    /* positive integer */

    if (val >= 0)
    {
	/* check resp. recalculate width */
	while (width <= 9 && val >= wrange[width]) {
	    width++;
	}
	/* number to string backwards */
	p = buf + width;
	*p-- = '\0';
	do {
	    register char c = (val % 10); val /= 10;
	    *p-- = '0' + c;
	} while (val > 0);
	/* fulling with pad-char */
	while (p >= buf) {
	    *p-- = padchar;
	}

	return buf + width;
    }
    /* negative integer */

    if (!width) width++;
    /* check resp. recalculate width (regarding sign) */
    width--;
    while (width <= 9 && val <= -wrange[width]) {
	width++;
    }
    width++;
    /* number to string backwards */
    p = buf + width;
    *p-- = '\0';
    /* differentiate platforms with -1 % 10 == 1 and -1 % 10 == -1 */
    if (-1 % 10 == -1) {
	do {
	    register char c = (val % 10); val /= 10;
	    *p-- = '0' - c;
	} while (val < 0);
    } else {
	do {
	    register char c = (val % 10); val /= 10;
	    *p-- = '0' + c;
	} while (val < 0);
    }
    /* sign by 0 padding */
    if (padchar != '0') { *p-- = '-'; }
    /* fulling with pad-char */
    while (p >= buf + 1) {
	*p-- = padchar;
    }
    /* sign by non 0 padding */
    if (padchar == '0') { *p = '-'; }

    return buf + width;
}
char *
TclItoAw(
    char *buf,
    int val,
    char  padchar,
    unsigned short int width)
{
    return _itoaw(buf, val, padchar, width);
}

static inline char *
_witoaw(
    char *buf,
    register Tcl_WideInt val,
    char  padchar,
    unsigned short int width)
{
    register char *p;
    static int wrange[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

    /* positive integer */

    if (val >= 0)
    {
	/* check resp. recalculate width */
	if (val >= 10000000000L) {
	    Tcl_WideInt val2;
	    val2 = val / 10000000000L;
	    while (width <= 9 && val2 >= wrange[width]) {
		width++;
	    }
	    width += 10;
	} else {
	    while (width <= 9 && val >= wrange[width]) {
		width++;
	    }
	}
	/* number to string backwards */
	p = buf + width;
	*p-- = '\0';
	do {
	    register char c = (val % 10); val /= 10;
	    *p-- = '0' + c;
	} while (val > 0);
	/* fulling with pad-char */
	while (p >= buf) {
	    *p-- = padchar;
	}

	return buf + width;
    }

    /* negative integer */

    if (!width) width++;
    /* check resp. recalculate width (regarding sign) */
    width--;
    if (val <= -10000000000L) {
	Tcl_WideInt val2;
	val2 = val / 10000000000L;
	while (width <= 9 && val2 <= -wrange[width]) {
	    width++;
	}
	width += 10;
    } else {
	while (width <= 9 && val <= -wrange[width]) {
	    width++;
	}
    }
    width++;
    /* number to string backwards */
    p = buf + width;
    *p-- = '\0';
    /* differentiate platforms with -1 % 10 == 1 and -1 % 10 == -1 */
    if (-1 % 10 == -1) {
	do {
	    register char c = (val % 10); val /= 10;
	    *p-- = '0' - c;
	} while (val < 0);
    } else {
	do {
	    register char c = (val % 10); val /= 10;
	    *p-- = '0' + c;
	} while (val < 0);
    }
    /* sign by 0 padding */
    if (padchar != '0') { *p-- = '-'; }
    /* fulling with pad-char */
    while (p >= buf + 1) {
	*p-- = padchar;
    }
    /* sign by non 0 padding */
    if (padchar == '0') { *p = '-'; }

    return buf + width;
}

/*
 * Global GC as LIFO for released scan/format object storages.
 *
 * Used to holds last released CLOCK_FMT_SCN_STORAGE_GC_SIZE formats
 * (after last reference from Tcl-object will be removed). This is helpful
 * to avoid continuous (re)creation and compiling by some dynamically resp.
 * variable format objects, that could be often reused.
 *
 * As long as format storage is used resp. belongs to GC, it takes place in
 * FmtScnHashTable also.
 */

#if CLOCK_FMT_SCN_STORAGE_GC_SIZE > 0

static struct {
    ClockFmtScnStorage	  *stackPtr;
    ClockFmtScnStorage	  *stackBound;
    unsigned int	   count;
} ClockFmtScnStorage_GC = {NULL, NULL, 0};

/*
 *----------------------------------------------------------------------
 *
 * ClockFmtScnStorageGC_In --
 *
 *	Adds an format storage object to GC.
 *
 *	If current GC is full (size larger as CLOCK_FMT_SCN_STORAGE_GC_SIZE)
 *	this removes last unused storage at begin of GC stack (LIFO).
 *
 *	Assumes caller holds the ClockFmtMutex.
 *
 * Results:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static inline void
ClockFmtScnStorageGC_In(ClockFmtScnStorage *entry)
{
    /* add new entry */
    TclSpliceIn(entry, ClockFmtScnStorage_GC.stackPtr);
    if (ClockFmtScnStorage_GC.stackBound == NULL) {
	ClockFmtScnStorage_GC.stackBound = entry;
    }
    ClockFmtScnStorage_GC.count++;

    /* if GC ist full */
    if (ClockFmtScnStorage_GC.count > CLOCK_FMT_SCN_STORAGE_GC_SIZE) {

	/* GC stack is LIFO: delete first inserted entry */
	ClockFmtScnStorage *delEnt = ClockFmtScnStorage_GC.stackBound;
	ClockFmtScnStorage_GC.stackBound = delEnt->prevPtr;
	TclSpliceOut(delEnt, ClockFmtScnStorage_GC.stackPtr);
	ClockFmtScnStorage_GC.count--;
	delEnt->prevPtr = delEnt->nextPtr = NULL;
	/* remove it now */
	ClockFmtScnStorageDelete(delEnt);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ClockFmtScnStorage_GC_Out --
 *
 *	Restores (for reusing) given format storage object from GC.
 *
 *	Assumes caller holds the ClockFmtMutex.
 *
 * Results:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static inline void
ClockFmtScnStorage_GC_Out(ClockFmtScnStorage *entry)
{
    TclSpliceOut(entry, ClockFmtScnStorage_GC.stackPtr);
    ClockFmtScnStorage_GC.count--;
    if (ClockFmtScnStorage_GC.stackBound == entry) {
	ClockFmtScnStorage_GC.stackBound = entry->prevPtr;
    }
    entry->prevPtr = entry->nextPtr = NULL;
}

#endif


/*
 * Global format storage hash table of type ClockFmtScnStorageHashKeyType
 * (contains list of scan/format object storages, shared across all threads).
 *
 * Used for fast searching by format string.
 */
static Tcl_HashTable FmtScnHashTable;
static int	     initialized = 0;

/*
 * Wrappers between pointers to hash entry and format storage object
 */
static inline Tcl_HashEntry *
HashEntry4FmtScn(ClockFmtScnStorage *fss) {
    return (Tcl_HashEntry*)(fss + 1);
};
static inline ClockFmtScnStorage *
FmtScn4HashEntry(Tcl_HashEntry *hKeyPtr) {
    return (ClockFmtScnStorage*)(((char*)hKeyPtr) - sizeof(ClockFmtScnStorage));
};

/*
 *----------------------------------------------------------------------
 *
 * ClockFmtScnStorageAllocProc --
 *
 *	Allocate space for a hash entry containing format storage together
 *	with the string key.
 *
 * Results:
 *	The return value is a pointer to the created entry.
 *
 *----------------------------------------------------------------------
 */

static Tcl_HashEntry *
ClockFmtScnStorageAllocProc(
    Tcl_HashTable *tablePtr,	/* Hash table. */
    void *keyPtr)		/* Key to store in the hash table entry. */
{
    ClockFmtScnStorage *fss;

    const char *string = (const char *) keyPtr;
    Tcl_HashEntry *hPtr;
    unsigned int size,
	allocsize = sizeof(ClockFmtScnStorage) + sizeof(Tcl_HashEntry);

    allocsize += (size = strlen(string) + 1);
    if (size > sizeof(hPtr->key)) {
	allocsize -= sizeof(hPtr->key);
    }

    fss = ckalloc(allocsize);

    /* initialize */
    memset(fss, 0, sizeof(*fss));

    hPtr = HashEntry4FmtScn(fss);
    memcpy(&hPtr->key.string, string, size);
    hPtr->clientData = 0; /* currently unused */

    return hPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * ClockFmtScnStorageFreeProc --
 *
 *	Free format storage object and space of given hash entry.
 *
 * Results:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
ClockFmtScnStorageFreeProc(
    Tcl_HashEntry *hPtr)
{
    ClockFmtScnStorage *fss = FmtScn4HashEntry(hPtr);

    if (fss->scnTok != NULL) {
	ckfree(fss->scnTok);
	fss->scnTok = NULL;
	fss->scnTokC = 0;
    }
    if (fss->fmtTok != NULL) {
	ckfree(fss->fmtTok);
	fss->fmtTok = NULL;
	fss->fmtTokC = 0;
    }

    ckfree(fss);
}

/*
 *----------------------------------------------------------------------
 *
 * ClockFmtScnStorageDelete --
 *
 *	Delete format storage object.
 *
 * Results:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
ClockFmtScnStorageDelete(ClockFmtScnStorage *fss) {
    Tcl_HashEntry   *hPtr = HashEntry4FmtScn(fss);
    /*
     * This will delete a hash entry and call "ckfree" for storage self, if
     * some additionally handling required, freeEntryProc can be used instead
     */
    Tcl_DeleteHashEntry(hPtr);
}


/*
 * Derivation of tclStringHashKeyType with another allocEntryProc
 */

static Tcl_HashKeyType ClockFmtScnStorageHashKeyType;


/*
 * Type definition of clock-format tcl object type.
 */

Tcl_ObjType ClockFmtObjType = {
    "clock-format",	       /* name */
    ClockFmtObj_FreeInternalRep, /* freeIntRepProc */
    ClockFmtObj_DupInternalRep,	 /* dupIntRepProc */
    ClockFmtObj_UpdateString,	 /* updateStringProc */
    ClockFmtObj_SetFromAny	 /* setFromAnyProc */
};

#define ObjClockFmtScn(objPtr) \
    (*((ClockFmtScnStorage **)&(objPtr)->internalRep.twoPtrValue.ptr1))

#define ObjLocFmtKey(objPtr) \
    (*((Tcl_Obj **)&(objPtr)->internalRep.twoPtrValue.ptr2))

static void
ClockFmtObj_DupInternalRep(
    Tcl_Obj *srcPtr,
    Tcl_Obj *copyPtr)
{
    ClockFmtScnStorage *fss = ObjClockFmtScn(srcPtr);

    if (fss != NULL) {
	Tcl_MutexLock(&ClockFmtMutex);
	fss->objRefCount++;
	Tcl_MutexUnlock(&ClockFmtMutex);
    }

    ObjClockFmtScn(copyPtr) = fss;
    /* regards special case - format not localizable */
    if (ObjLocFmtKey(srcPtr) != srcPtr) {
	Tcl_InitObjRef(ObjLocFmtKey(copyPtr), ObjLocFmtKey(srcPtr));
    } else {
	ObjLocFmtKey(copyPtr) = copyPtr;
    }
    copyPtr->typePtr = &ClockFmtObjType;


    /* if no format representation, dup string representation */
    if (fss == NULL) {
	copyPtr->bytes = ckalloc(srcPtr->length + 1);
	memcpy(copyPtr->bytes, srcPtr->bytes, srcPtr->length + 1);
	copyPtr->length = srcPtr->length;
    }
}

static void
ClockFmtObj_FreeInternalRep(
    Tcl_Obj *objPtr)
{
    ClockFmtScnStorage *fss = ObjClockFmtScn(objPtr);
    if (fss != NULL && initialized) {
	Tcl_MutexLock(&ClockFmtMutex);
	/* decrement object reference count of format/scan storage */
	if (--fss->objRefCount <= 0) {
	    #if CLOCK_FMT_SCN_STORAGE_GC_SIZE > 0
	      /* don't remove it right now (may be reusable), just add to GC */
	      ClockFmtScnStorageGC_In(fss);
	    #else
	      /* remove storage (format representation) */
	      ClockFmtScnStorageDelete(fss);
	    #endif
	}
	Tcl_MutexUnlock(&ClockFmtMutex);
    }
    ObjClockFmtScn(objPtr) = NULL;
    if (ObjLocFmtKey(objPtr) != objPtr) {
	Tcl_UnsetObjRef(ObjLocFmtKey(objPtr));
    } else {
	ObjLocFmtKey(objPtr) = NULL;
    }
    objPtr->typePtr = NULL;
};

static int
ClockFmtObj_SetFromAny(
    Tcl_Interp * interp,
    Tcl_Obj *objPtr)
{
    /* validate string representation before free old internal represenation */
    (void)TclGetString(objPtr);

    /* free old internal represenation */
    if (objPtr->typePtr && objPtr->typePtr->freeIntRepProc)
	objPtr->typePtr->freeIntRepProc(objPtr);

    /* initial state of format object */
    ObjClockFmtScn(objPtr) = NULL;
    ObjLocFmtKey(objPtr) = NULL;
    objPtr->typePtr = &ClockFmtObjType;

    return TCL_OK;
};

static void
ClockFmtObj_UpdateString(
    Tcl_Obj *objPtr)
{
    const char *name = "UNKNOWN";
    int	  len;
    ClockFmtScnStorage *fss = ObjClockFmtScn(objPtr);

    if (fss != NULL) {
	Tcl_HashEntry *hPtr = HashEntry4FmtScn(fss);
	name = hPtr->key.string;
    }
    len = strlen(name);
    objPtr->length = len,
    objPtr->bytes = ckalloc((size_t)++len);
    if (objPtr->bytes)
	memcpy(objPtr->bytes, name, len);
}

/*
 *----------------------------------------------------------------------
 *
 * ClockFrmObjGetLocFmtKey --
 *
 *	Retrieves format key object used to search localized format.
 *
 *	This is normally stored in second pointer of internal representation.
 *	If format object is not localizable, it is equal the given format
 *	pointer (special case to fast fallback by not-localizable formats).
 *
 * Results:
 *	Returns tcl object with key or format object if not localizable.
 *
 * Side effects:
 * 	Converts given format object to ClockFmtObjType on demand for caching
 *	the key inside its internal representation.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj*
ClockFrmObjGetLocFmtKey(
    Tcl_Interp *interp,
    Tcl_Obj    *objPtr)
{
    Tcl_Obj *keyObj;

    if (objPtr->typePtr != &ClockFmtObjType) {
	if (ClockFmtObj_SetFromAny(interp, objPtr) != TCL_OK) {
	    return NULL;
	}
    }

    keyObj = ObjLocFmtKey(objPtr);
    if (keyObj) {
	return keyObj;
    }

    keyObj = Tcl_ObjPrintf("FMT_%s", TclGetString(objPtr));
    Tcl_InitObjRef(ObjLocFmtKey(objPtr), keyObj);

    return keyObj;
}

/*
 *----------------------------------------------------------------------
 *
 * FindOrCreateFmtScnStorage --
 *
 *	Retrieves format storage for given string format.
 *
 *	This will find the given format in the global storage hash table
 *	or create a format storage object on demaind and save the
 *	reference in the first pointer of internal representation of given
 *	object.
 *
 * Results:
 *	Returns scan/format storage pointer to ClockFmtScnStorage.
 *
 * Side effects:
 * 	Converts given format object to ClockFmtObjType on demand for caching
 *	the format storage reference inside its internal representation.
 *	Increments objRefCount of the ClockFmtScnStorage reference.
 *
 *----------------------------------------------------------------------
 */

static ClockFmtScnStorage *
FindOrCreateFmtScnStorage(
    Tcl_Interp *interp,
    Tcl_Obj    *objPtr)
{
    const char *strFmt = TclGetString(objPtr);
    ClockFmtScnStorage *fss = NULL;
    int new;
    Tcl_HashEntry *hPtr;

    Tcl_MutexLock(&ClockFmtMutex);

    /* if not yet initialized */
    if (!initialized) {
	/* initialize type */
	memcpy(&ClockFmtScnStorageHashKeyType, &tclStringHashKeyType, sizeof(tclStringHashKeyType));
	ClockFmtScnStorageHashKeyType.allocEntryProc = ClockFmtScnStorageAllocProc;
	ClockFmtScnStorageHashKeyType.freeEntryProc = ClockFmtScnStorageFreeProc;

	/* initialize hash table */
	Tcl_InitCustomHashTable(&FmtScnHashTable, TCL_CUSTOM_TYPE_KEYS,
	    &ClockFmtScnStorageHashKeyType);

	initialized = 1;
    }

    /* get or create entry (and alocate storage) */
    hPtr = Tcl_CreateHashEntry(&FmtScnHashTable, strFmt, &new);
    if (hPtr != NULL) {

	fss = FmtScn4HashEntry(hPtr);

	#if CLOCK_FMT_SCN_STORAGE_GC_SIZE > 0
	  /* unlink if it is currently in GC */
	  if (new == 0 && fss->objRefCount == 0) {
	      ClockFmtScnStorage_GC_Out(fss);
	  }
	#endif

	/* new reference, so increment in lock right now */
	fss->objRefCount++;

	ObjClockFmtScn(objPtr) = fss;
    }

    Tcl_MutexUnlock(&ClockFmtMutex);

    if (fss == NULL && interp != NULL) {
	Tcl_AppendResult(interp, "retrieve clock format failed \"",
	    strFmt ? strFmt : "", "\"", NULL);
	Tcl_SetErrorCode(interp, "TCL", "EINVAL", NULL);
    }

    return fss;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetClockFrmScnFromObj --
 *
 *	Returns a clock format/scan representation of (*objPtr), if possible.
 *	If something goes wrong, NULL is returned, and if interp is non-NULL,
 *	an error message is written there.
 *
 * Results:
 *	Valid representation of type ClockFmtScnStorage.
 *
 * Side effects:
 *	Caches the ClockFmtScnStorage reference as the internal rep of (*objPtr)
 *	and in global hash table, shared across all threads.
 *
 *----------------------------------------------------------------------
 */

ClockFmtScnStorage *
Tcl_GetClockFrmScnFromObj(
    Tcl_Interp *interp,
    Tcl_Obj *objPtr)
{
    ClockFmtScnStorage *fss;

    if (objPtr->typePtr != &ClockFmtObjType) {
	if (ClockFmtObj_SetFromAny(interp, objPtr) != TCL_OK) {
	    return NULL;
	}
    }

    fss = ObjClockFmtScn(objPtr);

    if (fss == NULL) {
	fss = FindOrCreateFmtScnStorage(interp, objPtr);
    }

    return fss;
}
/*
 *----------------------------------------------------------------------
 *
 * ClockLocalizeFormat --
 *
 *	Wrap the format object in options to the localized format,
 *	corresponding given locale.
 *
 *	This searches localized format in locale catalog, and if not yet
 *	exists, it executes ::tcl::clock::LocalizeFormat in given interpreter
 *	and caches its result in the locale catalog.
 *
 * Results:
 *	Localized format object.
 *
 * Side effects:
 *	Caches the localized format inside locale catalog.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
ClockLocalizeFormat(
    ClockFmtScnCmdArgs *opts)
{
    ClockClientData *dataPtr = opts->clientData;
    Tcl_Obj *valObj = NULL, *keyObj;

    keyObj = ClockFrmObjGetLocFmtKey(opts->interp, opts->formatObj);

    /* special case - format object is not localizable */
    if (keyObj == opts->formatObj) {
	return opts->formatObj;
    }

    /* prevents loss of key object if the format object (where key stored)
     * becomes changed (loses its internal representation during evals) */
    Tcl_IncrRefCount(keyObj);

    if (opts->mcDictObj == NULL) {
	ClockMCDict(opts);
	if (opts->mcDictObj == NULL)
	    goto done;
    }

    /* try to find in cache within locale mc-catalog */
    if (Tcl_DictObjGet(NULL, opts->mcDictObj,
	    keyObj, &valObj) != TCL_OK) {
	goto done;
    }

    /* call LocalizeFormat locale format fmtkey */
    if (valObj == NULL) {
	Tcl_Obj *callargs[4];
	callargs[0] = dataPtr->literals[LIT_LOCALIZE_FORMAT];
	callargs[1] = opts->localeObj;
	callargs[2] = opts->formatObj;
	callargs[3] = opts->mcDictObj;
	if (Tcl_EvalObjv(opts->interp, 4, callargs, 0) == TCL_OK
	) {
	    valObj = Tcl_GetObjResult(opts->interp);
	}

	/* ensure mcDictObj remains unshared */
	if (opts->mcDictObj->refCount > 1) {
	    /* smart reference (shared dict as object with no ref-counter) */
	    opts->mcDictObj = Tcl_DictObjSmartRef(opts->interp,
		opts->mcDictObj);
	}
	if (!valObj) {
	    goto done;
	}
	/* cache it inside mc-dictionary (this incr. ref count of keyObj/valObj) */
	if (Tcl_DictObjPut(opts->interp, opts->mcDictObj,
		keyObj, valObj) != TCL_OK
	) {
	    valObj = NULL;
	    goto done;
	}

	Tcl_ResetResult(opts->interp);

	/* check special case - format object is not localizable */
	if (valObj == opts->formatObj) {
	    /* mark it as unlocalizable, by setting self as key (without refcount incr) */
	    if (valObj->typePtr == &ClockFmtObjType) {
		Tcl_UnsetObjRef(ObjLocFmtKey(valObj));
		ObjLocFmtKey(valObj) = valObj;
	    }
	}
    }

done:

    Tcl_UnsetObjRef(keyObj);
    return (opts->formatObj = valObj);
}

/*
 *----------------------------------------------------------------------
 *
 * FindTokenBegin --
 *
 *	Find begin of given scan token in string, corresponding token type.
 *
 * Results:
 *	Position of token inside string if found. Otherwise - end of string.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static const char *
FindTokenBegin(
    register const char *p,
    register const char *end,
    ClockScanToken *tok,
    int flags)
{
    char c;
    if (p < end) {
	/* next token a known token type */
	switch (tok->map->type) {
	case CTOKT_INT:
	case CTOKT_WIDE:
	    if (!(flags & CLF_STRICT)) {
		/* should match at least one digit or space */
		while (!isdigit(UCHAR(*p)) && !isspace(UCHAR(*p)) &&
			(p = Tcl_UtfNext(p)) < end) {}
	    } else {
		/* should match at least one digit */
		while (!isdigit(UCHAR(*p)) && (p = Tcl_UtfNext(p)) < end) {}
	    }
	    return p;
	case CTOKT_WORD:
	    c = *(tok->tokWord.start);
	    goto findChar;
	case CTOKT_SPACE:
	    while (!isspace(UCHAR(*p)) && (p = Tcl_UtfNext(p)) < end) {};
	    return p;
	case CTOKT_CHAR:
	    c = *((char *)tok->map->data);
findChar:
	    if (!(flags & CLF_STRICT)) {
		/* should match the char or space */
		while (*p != c && !isspace(UCHAR(*p)) &&
			(p = Tcl_UtfNext(p)) < end) {}
	    } else {
		/* should match the char */
		while (*p != c && (p = Tcl_UtfNext(p)) < end) {}
	    }
	    return p;
	}
    }
    return p;
}

/*
 *----------------------------------------------------------------------
 *
 * DetermineGreedySearchLen --
 *
 *	Determine min/max lengths as exact as possible (speed, greedy match).
 *
 * Results:
 *	None. Lengths are stored in *minLenPtr, *maxLenPtr.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
DetermineGreedySearchLen(
    ClockFmtScnCmdArgs *opts,
    DateInfo *info,
    ClockScanToken *tok,
    int *minLenPtr,
    int *maxLenPtr)
{
    register int minLen = tok->map->minSize;
    register int maxLen;
    register const char *p = yyInput + minLen,
			*end = info->dateEnd;

    /* if still tokens available, try to correct minimum length */
    if ((tok+1)->map) {
	end -= tok->endDistance + yySpaceCount;
	/* find position of next known token */
	p = FindTokenBegin(p, end, tok+1,
		TCL_CLOCK_FULL_COMPAT ? opts->flags : CLF_STRICT);
	if (p < end) {
	    minLen = p - yyInput;
	}
    }

    /* max length to the end regarding distance to end (min-width of following tokens) */
    maxLen = end - yyInput;
    /* several amendments */
    if (maxLen > tok->map->maxSize) {
	maxLen = tok->map->maxSize;
    };
    if (minLen < tok->map->minSize) {
	minLen = tok->map->minSize;
    }
    if (minLen > maxLen) {
	maxLen = minLen;
    }
    if (maxLen > info->dateEnd - yyInput) {
	maxLen = info->dateEnd - yyInput;
    }

    /* check digits rigth now */
    if (tok->map->type == CTOKT_INT || tok->map->type == CTOKT_WIDE) {
	p = yyInput;
	end = p + maxLen;
	if (end > info->dateEnd) { end = info->dateEnd; };
	while (isdigit(UCHAR(*p)) && p < end) { p++; };
	maxLen = p - yyInput;
    }

    /* try to get max length more precise for greedy match,
     * check the next ahead token available there */
    if (minLen < maxLen && tok->lookAhTok) {
	ClockScanToken *laTok = tok + tok->lookAhTok + 1;
	p = yyInput + maxLen;
	/* regards all possible spaces here (because they are optional) */
	end = p + tok->lookAhMax + yySpaceCount + 1;
	if (end > info->dateEnd) {
	    end = info->dateEnd;
	}
	p += tok->lookAhMin;
	if (laTok->map && p < end) {
	    /* try to find laTok between [lookAhMin, lookAhMax] */
	    while (minLen < maxLen) {
		const char *f = FindTokenBegin(p, end, laTok,
				    TCL_CLOCK_FULL_COMPAT ? opts->flags : CLF_STRICT);
		/* if found (not below lookAhMax) */
		if (f < end) {
		    break;
		}
		/* try again with fewer length */
		maxLen--;
		p--;
		end--;
	    }
	} else if (p > end) {
	    maxLen -= (p - end);
	    if (maxLen < minLen) {
		maxLen = minLen;
	    }
	}
    }

    *minLenPtr = minLen;
    *maxLenPtr = maxLen;
}

/*
 *----------------------------------------------------------------------
 *
 * ObjListSearch --
 *
 *	Find largest part of the input string from start regarding min and
 *	max lengths in the given list (utf-8, case sensitive).
 *
 * Results:
 *	TCL_OK - match found, TCL_RETURN - not matched, TCL_ERROR in error case.
 *
 * Side effects:
 *	Input points to end of the found token in string.
 *
 *----------------------------------------------------------------------
 */

static inline int
ObjListSearch(ClockFmtScnCmdArgs *opts,
    DateInfo *info, int *val,
    Tcl_Obj **lstv, int lstc,
    int minLen, int maxLen)
{
    int	       i, l, lf = -1;
    const char *s, *f, *sf;
    /* search in list */
    for (i = 0; i < lstc; i++) {
	s = TclGetString(lstv[i]);
	l = lstv[i]->length;

	if ( l >= minLen
	  && (f = TclUtfFindEqualNC(yyInput, yyInput + maxLen, s, s + l, &sf)) > yyInput
	) {
	    l = f - yyInput;
	    if (l < minLen) {
		continue;
	    }
	    /* found, try to find longest value (greedy search) */
	    if (l < maxLen && minLen != maxLen) {
		lf = i;
		minLen = l + 1;
		continue;
	    }
	    /* max possible - end of search */
	    *val = i;
	    yyInput += l;
	    break;
	}
    }

    /* if found */
    if (i < lstc) {
	return TCL_OK;
    }
    if (lf >= 0) {
	*val = lf;
	yyInput += minLen - 1;
	return TCL_OK;
    }
    return TCL_RETURN;
}
#if 0
/* currently unused */

static int
LocaleListSearch(ClockFmtScnCmdArgs *opts,
    DateInfo *info, int mcKey, int *val,
    int minLen, int maxLen)
{
    Tcl_Obj **lstv;
    int	      lstc;
    Tcl_Obj *valObj;

    /* get msgcat value */
    valObj = ClockMCGet(opts, mcKey);
    if (valObj == NULL) {
	return TCL_ERROR;
    }

    /* is a list */
    if (TclListObjGetElements(opts->interp, valObj, &lstc, &lstv) != TCL_OK) {
	return TCL_ERROR;
    }

    /* search in list */
    return ObjListSearch(opts, info, val, lstv, lstc,
	minLen, maxLen);
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * ClockMCGetListIdxTree --
 *
 *	Retrieves localized string indexed tree in the locale catalog for
 *	given literal index mcKey (and builds it on demand).
 *
 *	Searches localized index in locale catalog, and if not yet exists,
 *	creates string indexed tree and stores it in the locale catalog.
 *
 * Results:
 *	Localized string index tree.
 *
 * Side effects:
 *	Caches the localized string index tree inside locale catalog.
 *
 *----------------------------------------------------------------------
 */

static TclStrIdxTree *
ClockMCGetListIdxTree(
    ClockFmtScnCmdArgs *opts,
    int	 mcKey)
{
    TclStrIdxTree * idxTree;
    Tcl_Obj *objPtr = ClockMCGetIdx(opts, mcKey);
    if ( objPtr != NULL
      && (idxTree = TclStrIdxTreeGetFromObj(objPtr)) != NULL
    ) {
	return idxTree;

    } else {
	/* build new index */

	Tcl_Obj **lstv;
	int	  lstc;
	Tcl_Obj *valObj;

	objPtr = TclStrIdxTreeNewObj();
	if ((idxTree = TclStrIdxTreeGetFromObj(objPtr)) == NULL) {
	    goto done; /* unexpected, but ...*/
	}

	valObj = ClockMCGet(opts, mcKey);
	if (valObj == NULL) {
	    goto done;
	}

	if (TclListObjGetElements(opts->interp, valObj,
		&lstc, &lstv) != TCL_OK) {
	    goto done;
	};

	if (TclStrIdxTreeBuildFromList(idxTree, lstc, lstv, NULL) != TCL_OK) {
	    goto done;
	}

	ClockMCSetIdx(opts, mcKey, objPtr);
	objPtr = NULL;
    };

done:
    if (objPtr) {
	Tcl_DecrRefCount(objPtr);
	idxTree = NULL;
    }

    return idxTree;
}

/*
 *----------------------------------------------------------------------
 *
 * ClockMCGetMultiListIdxTree --
 *
 *	Retrieves localized string indexed tree in the locale catalog for
 *	multiple lists by literal indices mcKeys (and builds it on demand).
 *
 *	Searches localized index in locale catalog for mcKey, and if not
 *	yet exists, creates string indexed tree and stores it in the
 *	locale catalog.
 *
 * Results:
 *	Localized string index tree.
 *
 * Side effects:
 *	Caches the localized string index tree inside locale catalog.
 *
 *----------------------------------------------------------------------
 */

static TclStrIdxTree *
ClockMCGetMultiListIdxTree(
    ClockFmtScnCmdArgs *opts,
    int	 mcKey,
    int *mcKeys)
{
    TclStrIdxTree * idxTree;
    Tcl_Obj *objPtr = ClockMCGetIdx(opts, mcKey);
    if ( objPtr != NULL
      && (idxTree = TclStrIdxTreeGetFromObj(objPtr)) != NULL
    ) {
	return idxTree;

    } else {
	/* build new index */

	Tcl_Obj **lstv;
	int	  lstc;
	Tcl_Obj *valObj;

	objPtr = TclStrIdxTreeNewObj();
	if ((idxTree = TclStrIdxTreeGetFromObj(objPtr)) == NULL) {
	    goto done; /* unexpected, but ...*/
	}

	while (*mcKeys) {

	    valObj = ClockMCGet(opts, *mcKeys);
	    if (valObj == NULL) {
		goto done;
	    }

	    if (TclListObjGetElements(opts->interp, valObj,
		    &lstc, &lstv) != TCL_OK) {
		goto done;
	    };

	    if (TclStrIdxTreeBuildFromList(idxTree, lstc, lstv, NULL) != TCL_OK) {
		goto done;
	    }
	    mcKeys++;
	}

	ClockMCSetIdx(opts, mcKey, objPtr);
	objPtr = NULL;
    };

done:
    if (objPtr) {
	Tcl_DecrRefCount(objPtr);
	idxTree = NULL;
    }

    return idxTree;
}

/*
 *----------------------------------------------------------------------
 *
 * ClockStrIdxTreeSearch --
 *
 *	Find largest part of the input string from start regarding lengths
 *	in the given localized string indexed tree (utf-8, case sensitive).
 *
 * Results:
 *	TCL_OK - match found and the index stored in *val,
 *	TCL_RETURN - not matched or ambigous,
 * 	TCL_ERROR - in error case.
 *
 * Side effects:
 *	Input points to end of the found token in string.
 *
 *----------------------------------------------------------------------
 */

static inline int
ClockStrIdxTreeSearch(ClockFmtScnCmdArgs *opts,
    DateInfo *info, TclStrIdxTree *idxTree, int *val,
    int minLen, int maxLen)
{
    const char *f;
    TclStrIdx  *foundItem;
    f = TclStrIdxTreeSearch(NULL, &foundItem, idxTree,
	    yyInput, yyInput + maxLen);

    if (f <= yyInput || (f - yyInput) < minLen) {
	/* not found */
	return TCL_RETURN;
    }
    if (!foundItem->value) {
	/* ambigous */
	return TCL_RETURN;
    }

    *val = PTR2INT(foundItem->value);

    /* shift input pointer */
    yyInput = f;

    return TCL_OK;
}
#if 0
/* currently unused */

static int
StaticListSearch(ClockFmtScnCmdArgs *opts,
    DateInfo *info, const char **lst, int *val)
{
    int len;
    const char **s = lst;
    while (*s != NULL) {
	len = strlen(*s);
	if ( len <= info->dateEnd - yyInput
	  && strncasecmp(yyInput, *s, len) == 0
	) {
	    *val = (s - lst);
	    yyInput += len;
	    break;
	}
	s++;
    }
    if (*s != NULL) {
	return TCL_OK;
    }
    return TCL_RETURN;
}
#endif

static inline const char *
FindWordEnd(
    ClockScanToken *tok,
    register const char * p, const char * end)
{
    register const char *x = tok->tokWord.start;
    const char *pfnd = p;
    if (x == tok->tokWord.end - 1) { /* fast phase-out for single char word */
	if (*p == *x) {
	    return ++p;
	}
    }
    /* multi-char word */
    x = TclUtfFindEqualNC(x, tok->tokWord.end, p, end, &pfnd);
    if (x < tok->tokWord.end) {
	/* no match -> error */
	return NULL;
    }
    return pfnd;
}

static int
ClockScnToken_Month_Proc(ClockFmtScnCmdArgs *opts,
    DateInfo *info, ClockScanToken *tok)
{
#if 0
/* currently unused, test purposes only */
    static const char * months[] = {
	/* full */
	"January", "February", "March",
	"April",   "May",      "June",
	"July",	   "August",   "September",
	"October", "November", "December",
	/* abbr */
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
	NULL
    };
    int val;
    if (StaticListSearch(opts, info, months, &val) != TCL_OK) {
	return TCL_RETURN;
    }
    yyMonth = (val % 12) + 1;
    return TCL_OK;
#endif

    static int monthsKeys[] = {MCLIT_MONTHS_FULL, MCLIT_MONTHS_ABBREV, 0};

    int ret, val;
    int minLen, maxLen;
    TclStrIdxTree *idxTree;

    DetermineGreedySearchLen(opts, info, tok, &minLen, &maxLen);

    /* get or create tree in msgcat dict */

    idxTree = ClockMCGetMultiListIdxTree(opts, MCLIT_MONTHS_COMB, monthsKeys);
    if (idxTree == NULL) {
	return TCL_ERROR;
    }

    ret = ClockStrIdxTreeSearch(opts, info, idxTree, &val, minLen, maxLen);
    if (ret != TCL_OK) {
	return ret;
    }

    yyMonth = val;
    return TCL_OK;

}

static int
ClockScnToken_DayOfWeek_Proc(ClockFmtScnCmdArgs *opts,
    DateInfo *info, ClockScanToken *tok)
{
    static int dowKeys[] = {MCLIT_DAYS_OF_WEEK_ABBREV, MCLIT_DAYS_OF_WEEK_FULL, 0};

    int ret, val;
    int minLen, maxLen;
    char curTok = *tok->tokWord.start;
    TclStrIdxTree *idxTree;

    DetermineGreedySearchLen(opts, info, tok, &minLen, &maxLen);

    /* %u %w %Ou %Ow */
    if ( curTok != 'a' && curTok != 'A'
      && ((minLen <= 1 && maxLen >= 1) || PTR2INT(tok->map->data))
    ) {

	val = -1;

	if (PTR2INT(tok->map->data) == 0) {
	    if (*yyInput >= '0' && *yyInput <= '9') {
		val = *yyInput - '0';
	    }
	} else {
	    idxTree = ClockMCGetListIdxTree(opts, PTR2INT(tok->map->data) /* mcKey */);
	    if (idxTree == NULL) {
		return TCL_ERROR;
	    }

	    ret = ClockStrIdxTreeSearch(opts, info, idxTree, &val, minLen, maxLen);
	    if (ret != TCL_OK) {
		return ret;
	    }
	    --val;
	}

	if (val != -1) {
	    if (val == 0) {
		val = 7;
	    }
	    if (val > 7) {
		Tcl_SetObjResult(opts->interp, Tcl_NewStringObj("day of week is greater than 7", -1));
		Tcl_SetErrorCode(opts->interp, "CLOCK", "badDayOfWeek", NULL);
		return TCL_ERROR;
	    }
	    info->date.dayOfWeek = val;
	    yyInput++;
	    return TCL_OK;
	}


	return TCL_RETURN;
    }

    /* %a %A */
    idxTree = ClockMCGetMultiListIdxTree(opts, MCLIT_DAYS_OF_WEEK_COMB, dowKeys);
    if (idxTree == NULL) {
	return TCL_ERROR;
    }

    ret = ClockStrIdxTreeSearch(opts, info, idxTree, &val, minLen, maxLen);
    if (ret != TCL_OK) {
	return ret;
    }
    --val;

    if (val == 0) {
	val = 7;
    }
    info->date.dayOfWeek = val;
    return TCL_OK;

}

static int
ClockScnToken_amPmInd_Proc(ClockFmtScnCmdArgs *opts,
    DateInfo *info, ClockScanToken *tok)
{
    int ret, val;
    int minLen, maxLen;
    Tcl_Obj *amPmObj[2];

    DetermineGreedySearchLen(opts, info, tok, &minLen, &maxLen);

    amPmObj[0] = ClockMCGet(opts, MCLIT_AM);
    amPmObj[1] = ClockMCGet(opts, MCLIT_PM);

    if (amPmObj[0] == NULL || amPmObj[1] == NULL) {
	return TCL_ERROR;
    }

    ret = ObjListSearch(opts, info, &val, amPmObj, 2,
	minLen, maxLen);
    if (ret != TCL_OK) {
	return ret;
    }

    if (val == 0) {
	yyMeridian = MERam;
    } else {
	yyMeridian = MERpm;
    }

    return TCL_OK;
}

static int
ClockScnToken_LocaleERA_Proc(ClockFmtScnCmdArgs *opts,
    DateInfo *info, ClockScanToken *tok)
{
    ClockClientData *dataPtr = opts->clientData;

    int ret, val;
    int minLen, maxLen;
    Tcl_Obj *eraObj[6];

    DetermineGreedySearchLen(opts, info, tok, &minLen, &maxLen);

    eraObj[0] = ClockMCGet(opts, MCLIT_BCE);
    eraObj[1] = ClockMCGet(opts, MCLIT_CE);
    eraObj[2] = dataPtr->mcLiterals[MCLIT_BCE2];
    eraObj[3] = dataPtr->mcLiterals[MCLIT_CE2];
    eraObj[4] = dataPtr->mcLiterals[MCLIT_BCE3];
    eraObj[5] = dataPtr->mcLiterals[MCLIT_CE3];

    if (eraObj[0] == NULL || eraObj[1] == NULL) {
	return TCL_ERROR;
    }

    ret = ObjListSearch(opts, info, &val, eraObj, 6,
	minLen, maxLen);
    if (ret != TCL_OK) {
	return ret;
    }

    if (val & 1) {
	yydate.era = CE;
    } else {
	yydate.era = BCE;
    }

    return TCL_OK;
}

static int
ClockScnToken_LocaleListMatcher_Proc(ClockFmtScnCmdArgs *opts,
    DateInfo *info, ClockScanToken *tok)
{
    int ret, val;
    int minLen, maxLen;
    TclStrIdxTree *idxTree;

    DetermineGreedySearchLen(opts, info, tok, &minLen, &maxLen);

    /* get or create tree in msgcat dict */

    idxTree = ClockMCGetListIdxTree(opts, PTR2INT(tok->map->data) /* mcKey */);
    if (idxTree == NULL) {
	return TCL_ERROR;
    }

    ret = ClockStrIdxTreeSearch(opts, info, idxTree, &val, minLen, maxLen);
    if (ret != TCL_OK) {
	return ret;
    }

    if (tok->map->offs > 0) {
	*(int *)(((char *)info) + tok->map->offs) = --val;
    }

    return TCL_OK;
}

static int
ClockScnToken_JDN_Proc(ClockFmtScnCmdArgs *opts,
    DateInfo *info, ClockScanToken *tok)
{
    int minLen, maxLen;
    register const char *p = yyInput, *end; const char *s;
    Tcl_WideInt intJD; int fractJD = 0, fractJDDiv = 1;

    DetermineGreedySearchLen(opts, info, tok, &minLen, &maxLen);

    end = yyInput + maxLen;

    /* currently positive astronomic dates only */
    if (*p == '+' || *p == '-') { p++; };
    s = p;
    while (p < end && isdigit(UCHAR(*p))) {
	p++;
    }
    if ( _str2wideInt(&intJD, s, p, (*yyInput != '-' ? 1 : -1)) != TCL_OK) {
	return TCL_RETURN;
    };
    yyInput = p;
    if (p >= end || *p++ != '.') { /* allow pure integer JDN */
	/* by astronomical JD the seconds of day offs is 12 hours */
	if (tok->map->offs) {
	    goto done;
	}
	/* calendar JD */
	yydate.julianDay = intJD;
	return TCL_OK;
    }
    s = p;
    while (p < end && isdigit(UCHAR(*p))) {
    	fractJDDiv *= 10;
	p++;
    }
    if ( _str2int(&fractJD, s, p, 1) != TCL_OK) {
	return TCL_RETURN;
    };
    yyInput = p;

done:
    /* 
     * Build a date from julian day (integer and fraction).
     * Note, astronomical JDN starts at noon in opposite to calendar julianday.
     */

    fractJD = (int)tok->map->offs /* 0 for calendar or 43200 for astro JD */
	+ (int)((Tcl_WideInt)SECONDS_PER_DAY * fractJD / fractJDDiv);
    if (fractJD >= SECONDS_PER_DAY) {
	fractJD %= SECONDS_PER_DAY;
	intJD += 1;
    }
    yydate.secondOfDay = fractJD;
    yydate.julianDay = intJD;

    yydate.seconds =
	-210866803200L
	+ ( SECONDS_PER_DAY * intJD )
	+ ( fractJD );

    info->flags |= CLF_POSIXSEC;

    return TCL_OK;
}

static int
ClockScnToken_TimeZone_Proc(ClockFmtScnCmdArgs *opts,
    DateInfo *info, ClockScanToken *tok)
{
    int minLen, maxLen;
    int len = 0;
    register const char *p = yyInput;
    Tcl_Obj *tzObjStor = NULL;

    DetermineGreedySearchLen(opts, info, tok, &minLen, &maxLen);

    /* numeric timezone */
    if (*p == '+' || *p == '-') {
	/* max chars in numeric zone = "+00:00:00" */
    #define MAX_ZONE_LEN 9
	char buf[MAX_ZONE_LEN + 1];
	char *bp = buf;
	*bp++ = *p++; len++;
	if (maxLen > MAX_ZONE_LEN)
	    maxLen = MAX_ZONE_LEN;
	/* cumulate zone into buf without ':' */
	while (len + 1 < maxLen) {
	    if (!isdigit(UCHAR(*p))) break;
	    *bp++ = *p++; len++;
	    if (!isdigit(UCHAR(*p))) break;
	    *bp++ = *p++; len++;
	    if (len + 2 < maxLen) {
		if (*p == ':') {
		    p++; len++;
		}
	    }
	}
	*bp = '\0';

	if (len < minLen) {
	    return TCL_RETURN;
	}
    #undef MAX_ZONE_LEN

	/* timezone */
	tzObjStor = Tcl_NewStringObj(buf, bp-buf);
    } else {
	/* legacy (alnum) timezone like CEST, etc. */
	if (maxLen > 4)
	    maxLen = 4;
	while (len < maxLen) {
	    if ( (*p & 0x80)
	      || (!isalpha(UCHAR(*p)) && !isdigit(UCHAR(*p)))
	    ) {	      /* INTL: ISO only. */
		break;
	    }
	    p++; len++;
	}

	if (len < minLen) {
	    return TCL_RETURN;
	}

	/* timezone */
	tzObjStor = Tcl_NewStringObj(yyInput, p-yyInput);

	/* convert using dict */
    }

    /* try to apply new time zone */
    Tcl_IncrRefCount(tzObjStor);

    opts->timezoneObj = ClockSetupTimeZone(opts->clientData, opts->interp,
	tzObjStor);

    Tcl_DecrRefCount(tzObjStor);
    if (opts->timezoneObj == NULL) {
	return TCL_ERROR;
    }

    yyInput += len;

    return TCL_OK;
}

static int
ClockScnToken_StarDate_Proc(ClockFmtScnCmdArgs *opts,
    DateInfo *info, ClockScanToken *tok)
{
    int minLen, maxLen;
    register const char *p = yyInput, *end; const char *s;
    int year, fractYear, fractDayDiv, fractDay;
    static const char *stardatePref = "stardate ";

    DetermineGreedySearchLen(opts, info, tok, &minLen, &maxLen);

    end = yyInput + maxLen;

    /* stardate string */
    p = TclUtfFindEqualNCInLwr(p, end, stardatePref, stardatePref + 9, &s);
    if (p >= end || p - yyInput < 9) {
	return TCL_RETURN;
    }
    /* bypass spaces */
    while (p < end && isspace(UCHAR(*p))) {
	p++;
    }
    if (p >= end) {
	return TCL_RETURN;
    }
    /* currently positive stardate only */
    if (*p == '+') { p++; };
    s = p;
    while (p < end && isdigit(UCHAR(*p))) {
	p++;
    }
    if (p >= end || p - s < 4) {
	return TCL_RETURN;
    }
    if ( _str2int(&year, s, p-3, 1) != TCL_OK
      || _str2int(&fractYear, p-3, p, 1) != TCL_OK) {
	return TCL_RETURN;
    };
    if (*p++ != '.') {
	return TCL_RETURN;
    }
    s = p;
    fractDayDiv = 1;
    while (p < end && isdigit(UCHAR(*p))) {
	fractDayDiv *= 10;
	p++;
    }
    if ( _str2int(&fractDay, s, p, 1) != TCL_OK) {
	return TCL_RETURN;
    };
    yyInput = p;

    /* Build a date from year and fraction. */

    yydate.year = year + RODDENBERRY;
    yydate.era = CE;
    yydate.gregorian = 1;

    if (IsGregorianLeapYear(&yydate)) {
	fractYear *= 366;
    } else {
	fractYear *= 365;
    }
    yydate.dayOfYear = fractYear / 1000 + 1;
    if (fractYear % 1000 >= 500) {
	yydate.dayOfYear++;
    }

    GetJulianDayFromEraYearDay(&yydate, GREGORIAN_CHANGE_DATE);

    yydate.localSeconds =
	-210866803200L
	+ ( SECONDS_PER_DAY * yydate.julianDay )
	+ ( SECONDS_PER_DAY * fractDay / fractDayDiv );

    return TCL_OK;
}

static const char *ScnSTokenMapIndex =
    "dmbyYHMSpJjCgGVazUsntQ";
static ClockScanTokenMap ScnSTokenMap[] = {
    /* %d %e */
    {CTOKT_INT, CLF_DAYOFMONTH, 0, 1, 2, TclOffset(DateInfo, date.dayOfMonth),
	NULL},
    /* %m %N */
    {CTOKT_INT, CLF_MONTH, 0, 1, 2, TclOffset(DateInfo, date.month),
	NULL},
    /* %b %B %h */
    {CTOKT_PARSER, CLF_MONTH, 0, 0, 0xffff, 0,
	    ClockScnToken_Month_Proc, NULL},
    /* %y */
    {CTOKT_INT, CLF_YEAR, 0, 1, 2, TclOffset(DateInfo, date.year),
	NULL},
    /* %Y */
    {CTOKT_INT, CLF_YEAR | CLF_CENTURY, 0, 4, 4, TclOffset(DateInfo, date.year),
	NULL},
    /* %H %k %I %l */
    {CTOKT_INT, CLF_TIME, 0, 1, 2, TclOffset(DateInfo, date.hour),
	NULL},
    /* %M */
    {CTOKT_INT, CLF_TIME, 0, 1, 2, TclOffset(DateInfo, date.minutes),
	NULL},
    /* %S */
    {CTOKT_INT, CLF_TIME, 0, 1, 2, TclOffset(DateInfo, date.secondOfMin),
	NULL},
    /* %p %P */
    {CTOKT_PARSER, 0, 0, 0, 0xffff, 0,
	ClockScnToken_amPmInd_Proc, NULL},
    /* %J */
    {CTOKT_WIDE, CLF_JULIANDAY | CLF_SIGNED, 0, 1, 0xffff, TclOffset(DateInfo, date.julianDay),
	NULL},
    /* %j */
    {CTOKT_INT, CLF_DAYOFYEAR, 0, 1, 3, TclOffset(DateInfo, date.dayOfYear),
	NULL},
    /* %C */
    {CTOKT_INT, CLF_CENTURY|CLF_ISO8601CENTURY, 0, 1, 2, TclOffset(DateInfo, dateCentury),
	NULL},
    /* %g */
    {CTOKT_INT, CLF_ISO8601YEAR, 0, 2, 2, TclOffset(DateInfo, date.iso8601Year),
	NULL},
    /* %G */
    {CTOKT_INT, CLF_ISO8601YEAR | CLF_ISO8601CENTURY, 0, 4, 4, TclOffset(DateInfo, date.iso8601Year),
	NULL},
    /* %V */
    {CTOKT_INT, CLF_ISO8601WEAK, 0, 1, 2, TclOffset(DateInfo, date.iso8601Week),
	NULL},
    /* %a %A %u %w */
    {CTOKT_PARSER, CLF_DAYOFWEEK, 0, 0, 0xffff, 0,
	ClockScnToken_DayOfWeek_Proc, NULL},
    /* %z %Z */
    {CTOKT_PARSER, CLF_OPTIONAL, 0, 0, 0xffff, 0,
	ClockScnToken_TimeZone_Proc, NULL},
    /* %U %W */
    {CTOKT_INT, CLF_OPTIONAL, 0, 1, 2, 0, /* currently no capture, parse only token */
	NULL},
    /* %s */
    {CTOKT_WIDE, CLF_POSIXSEC | CLF_SIGNED, 0, 1, 0xffff, TclOffset(DateInfo, date.seconds),
	NULL},
    /* %n */
    {CTOKT_CHAR, 0, 0, 1, 1, 0, NULL, "\n"},
    /* %t */
    {CTOKT_CHAR, 0, 0, 1, 1, 0, NULL, "\t"},
    /* %Q */
    {CTOKT_PARSER, CLF_LOCALSEC, 0, 16, 30, 0,
	ClockScnToken_StarDate_Proc, NULL},
};
static const char *ScnSTokenMapAliasIndex[2] = {
    "eNBhkIlPAuwZW",
    "dmbbHHHpaaazU"
};

static const char *ScnETokenMapIndex =
    "EJjys";
static ClockScanTokenMap ScnETokenMap[] = {
    /* %EE */
    {CTOKT_PARSER, 0, 0, 0, 0xffff, TclOffset(DateInfo, date.year),
	ClockScnToken_LocaleERA_Proc, (void *)MCLIT_LOCALE_NUMERALS},
    /* %EJ */
    {CTOKT_PARSER, CLF_JULIANDAY | CLF_SIGNED, 0, 1, 0xffff, 0, /* calendar JDN starts at midnight */
	ClockScnToken_JDN_Proc, NULL},
    /* %Ej */
    {CTOKT_PARSER, CLF_JULIANDAY | CLF_SIGNED, 0, 1, 0xffff, (SECONDS_PER_DAY/2), /* astro JDN starts at noon */
	ClockScnToken_JDN_Proc, NULL},
    /* %Ey */
    {CTOKT_PARSER, 0, 0, 0, 0xffff, 0, /* currently no capture, parse only token */
	ClockScnToken_LocaleListMatcher_Proc, (void *)MCLIT_LOCALE_NUMERALS},
    /* %Es */
    {CTOKT_WIDE, CLF_LOCALSEC | CLF_SIGNED, 0, 1, 0xffff, TclOffset(DateInfo, date.localSeconds),
	NULL},
};
static const char *ScnETokenMapAliasIndex[2] = {
    "",
    ""
};

static const char *ScnOTokenMapIndex =
    "dmyHMSu";
static ClockScanTokenMap ScnOTokenMap[] = {
    /* %Od %Oe */
    {CTOKT_PARSER, CLF_DAYOFMONTH, 0, 0, 0xffff, TclOffset(DateInfo, date.dayOfMonth),
	ClockScnToken_LocaleListMatcher_Proc, (void *)MCLIT_LOCALE_NUMERALS},
    /* %Om */
    {CTOKT_PARSER, CLF_MONTH, 0, 0, 0xffff, TclOffset(DateInfo, date.month),
	ClockScnToken_LocaleListMatcher_Proc, (void *)MCLIT_LOCALE_NUMERALS},
    /* %Oy */
    {CTOKT_PARSER, CLF_YEAR, 0, 0, 0xffff, TclOffset(DateInfo, date.year),
	ClockScnToken_LocaleListMatcher_Proc, (void *)MCLIT_LOCALE_NUMERALS},
    /* %OH %Ok %OI %Ol */
    {CTOKT_PARSER, CLF_TIME, 0, 0, 0xffff, TclOffset(DateInfo, date.hour),
	ClockScnToken_LocaleListMatcher_Proc, (void *)MCLIT_LOCALE_NUMERALS},
    /* %OM */
    {CTOKT_PARSER, CLF_TIME, 0, 0, 0xffff, TclOffset(DateInfo, date.minutes),
	ClockScnToken_LocaleListMatcher_Proc, (void *)MCLIT_LOCALE_NUMERALS},
    /* %OS */
    {CTOKT_PARSER, CLF_TIME, 0, 0, 0xffff, TclOffset(DateInfo, date.secondOfMin),
	ClockScnToken_LocaleListMatcher_Proc, (void *)MCLIT_LOCALE_NUMERALS},
    /* %Ou Ow */
    {CTOKT_PARSER, CLF_DAYOFWEEK, 0, 0, 0xffff, 0,
	ClockScnToken_DayOfWeek_Proc, (void *)MCLIT_LOCALE_NUMERALS},
};
static const char *ScnOTokenMapAliasIndex[2] = {
    "ekIlw",
    "dHHHu"
};

/* Token map reserved for CTOKT_SPACE */
static ClockScanTokenMap ScnSpaceTokenMap = {
    CTOKT_SPACE,  0, 0, 1, 1, 0,
	NULL,
};

static ClockScanTokenMap ScnWordTokenMap = {
    CTOKT_WORD,	   0, 0, 1, 1, 0,
	NULL
};


static inline unsigned int
EstimateTokenCount(
    register const char *fmt,
    register const char *end)
{
    register const char *p = fmt;
    unsigned int tokcnt;
    /* estimate token count by % char and format length */
    tokcnt = 0;
    while (p <= end) {
	if (*p++ == '%') {
	    tokcnt++;
	    p++;
	}
    }
    p = fmt + tokcnt * 2;
    if (p < end) {
	if ((unsigned int)(end - p) < tokcnt) {
	    tokcnt += (end - p);
	} else {
	    tokcnt += tokcnt;
	}
    }
    return ++tokcnt;
}

#define AllocTokenInChain(tok, chain, tokCnt)				 \
    if (++(tok) >= (chain) + (tokCnt)) {				 \
	chain = ckrealloc((char *)(chain),				 \
	    (tokCnt + CLOCK_MIN_TOK_CHAIN_BLOCK_SIZE) * sizeof(*(tok))); \
	(tok) = (chain) + (tokCnt);					 \
	(tokCnt) += CLOCK_MIN_TOK_CHAIN_BLOCK_SIZE;			 \
    }									 \
    memset(tok, 0, sizeof(*(tok)));

/*
 *----------------------------------------------------------------------
 */
ClockFmtScnStorage *
ClockGetOrParseScanFormat(
    Tcl_Interp *interp,		/* Tcl interpreter */
    Tcl_Obj    *formatObj)	/* Format container */
{
    ClockFmtScnStorage *fss;

    fss = Tcl_GetClockFrmScnFromObj(interp, formatObj);
    if (fss == NULL) {
	return NULL;
    }

    /* if format (scnTok) already tokenized */
    if (fss->scnTok != NULL) {
	return fss;
    }

    Tcl_MutexLock(&ClockFmtMutex);

    /* first time scanning - tokenize format */
    if (fss->scnTok == NULL) {
	ClockScanToken *tok, *scnTok;
	unsigned int tokCnt;
	register const char *p, *e, *cp;

	e = p = HashEntry4FmtScn(fss)->key.string;
	e += strlen(p);

	/* estimate token count by % char and format length */
	fss->scnTokC = EstimateTokenCount(p, e);

	fss->scnSpaceCount = 0;

	scnTok = tok = ckalloc(sizeof(*tok) * fss->scnTokC);
	memset(tok, 0, sizeof(*(tok)));
	tokCnt = 1;
	while (p < e) {
	    switch (*p) {
	    case '%':
	    if (1) {
		ClockScanTokenMap * scnMap = ScnSTokenMap;
		const char *mapIndex =	ScnSTokenMapIndex,
			  **aliasIndex = ScnSTokenMapAliasIndex;
		if (p+1 >= e) {
		    goto word_tok;
		}
		p++;
		/* try to find modifier: */
		switch (*p) {
		case '%':
		    /* begin new word token - don't join with previous word token,
		     * because current mapping should be "...%%..." -> "...%..." */
		    tok->map = &ScnWordTokenMap;
		    tok->tokWord.start = p;
		    tok->tokWord.end = p+1;
		    AllocTokenInChain(tok, scnTok, fss->scnTokC); tokCnt++;
		    p++;
		    continue;
		break;
		case 'E':
		    scnMap = ScnETokenMap,
		    mapIndex =	ScnETokenMapIndex,
		    aliasIndex = ScnETokenMapAliasIndex;
		    p++;
		break;
		case 'O':
		    scnMap = ScnOTokenMap,
		    mapIndex = ScnOTokenMapIndex,
		    aliasIndex = ScnOTokenMapAliasIndex;
		    p++;
		break;
		}
		/* search direct index */
		cp = strchr(mapIndex, *p);
		if (!cp || *cp == '\0') {
		    /* search wrapper index (multiple chars for same token) */
		    cp = strchr(aliasIndex[0], *p);
		    if (!cp || *cp == '\0') {
			p--; if (scnMap != ScnSTokenMap) p--;
			goto word_tok;
		    }
		    cp = strchr(mapIndex, aliasIndex[1][cp - aliasIndex[0]]);
		    if (!cp || *cp == '\0') { /* unexpected, but ... */
		    #ifdef DEBUG
			Tcl_Panic("token \"%c\" has no map in wrapper resolver", *p);
		    #endif
			p--; if (scnMap != ScnSTokenMap) p--;
			goto word_tok;
		    }
		}
		tok->map = &scnMap[cp - mapIndex];
		tok->tokWord.start = p;

		/* calculate look ahead value by standing together tokens */
		if (tok > scnTok) {
		    ClockScanToken     *prevTok = tok - 1;

		    while (prevTok >= scnTok) {
			if (prevTok->map->type != tok->map->type) {
			    break;
			}
			prevTok->lookAhMin += tok->map->minSize;
			prevTok->lookAhMax += tok->map->maxSize;
			prevTok->lookAhTok++;
			prevTok--;
		    }
		}

		/* increase space count used in format */
		if ( tok->map->type == CTOKT_CHAR
		  && isspace(UCHAR(*((char *)tok->map->data)))
		) {
		    fss->scnSpaceCount++;
		}

		/* next token */
		AllocTokenInChain(tok, scnTok, fss->scnTokC); tokCnt++;
		p++;
		continue;
	    }
	    break;
	    default:
		if (isspace(UCHAR(*p))) {
		    tok->map = &ScnSpaceTokenMap;
		    tok->tokWord.start = p++;
		    while (p < e && isspace(UCHAR(*p))) {
			p++;
		    }
		    tok->tokWord.end = p;
		    /* increase space count used in format */
		    fss->scnSpaceCount++;
		    /* next token */
		    AllocTokenInChain(tok, scnTok, fss->scnTokC); tokCnt++;
		    continue;
		}
	      word_tok:
		{
		/* try continue with previous word token */
		ClockScanToken *wordTok = tok - 1;

		if (wordTok < scnTok || wordTok->map != &ScnWordTokenMap) {
		    /* start with new word token */
		    wordTok = tok;
		    wordTok->tokWord.start = p;
		    wordTok->map = &ScnWordTokenMap;
		}
		
		do {
		    if (isspace(UCHAR(*p))) {
			fss->scnSpaceCount++;
		    }
		    p = Tcl_UtfNext(p);
		} while (p < e && *p != '%');
		wordTok->tokWord.end = p;

		if (wordTok == tok) {
		    AllocTokenInChain(tok, scnTok, fss->scnTokC); tokCnt++;
		}


		}


	    break;
	    }
	}

	/* calculate end distance value for each tokens */
	if (tok > scnTok) {
	    unsigned int	endDist = 0;
	    ClockScanToken     *prevTok = tok-1;

	    while (prevTok >= scnTok) {
		prevTok->endDistance = endDist;
		if (prevTok->map->type != CTOKT_WORD) {
		    endDist += prevTok->map->minSize;
		} else {
		    endDist += prevTok->tokWord.end - prevTok->tokWord.start;
		}
		prevTok--;
	    }
	}

	/* correct count of real used tokens and free mem if desired
	 * (1 is acceptable delta to prevent memory fragmentation) */
	if (fss->scnTokC > tokCnt + (CLOCK_MIN_TOK_CHAIN_BLOCK_SIZE / 2)) {
	    if ( (tok = ckrealloc(scnTok, tokCnt * sizeof(*tok))) != NULL ) {
		scnTok = tok;
	    }
	}

	/* now we're ready - assign now to storage (note the threaded race condition) */
	fss->scnTok = scnTok;
	fss->scnTokC = tokCnt;
    }

    Tcl_MutexUnlock(&ClockFmtMutex);
    return fss;
}

/*
 *----------------------------------------------------------------------
 */
int
ClockScan(
    register DateInfo *info,	/* Date fields used for parsing & converting */
    Tcl_Obj *strObj,		/* String containing the time to scan */
    ClockFmtScnCmdArgs *opts)	/* Command options */
{
    ClockClientData *dataPtr = opts->clientData;
    ClockFmtScnStorage	*fss;
    ClockScanToken	*tok;
    ClockScanTokenMap	*map;
    register const char *p, *x, *end;
    unsigned short int	 flags = 0;
    int ret = TCL_ERROR;

    /* get localized format */
    if (ClockLocalizeFormat(opts) == NULL) {
	return TCL_ERROR;
    }

    if ( !(fss = ClockGetOrParseScanFormat(opts->interp, opts->formatObj))
      || !(tok = fss->scnTok)
    ) {
	return TCL_ERROR;
    }

    /* prepare parsing */

    yyMeridian = MER24;

    p = TclGetString(strObj);
    end = p + strObj->length;
    /* in strict mode - bypass spaces at begin / end only (not between tokens) */
    if (opts->flags & CLF_STRICT) {
	while (p < end && isspace(UCHAR(*p))) {
	    p++;
	}
    }
    yyInput = p;
    /* look ahead to count spaces (bypass it by count length and distances) */
    x = end;
    while (p < end) {
	if (isspace(UCHAR(*p))) {
	    x = ++p; /* after first space in space block */
	    yySpaceCount++;
	    while (p < end && isspace(UCHAR(*p))) {
		p++;
		yySpaceCount++;
	    }
	    continue;
	}
	x = end;
	p++;
    }
    /* ignore more as 1 space at end */
    yySpaceCount -= (end - x);
    end = x;
    /* ignore mandatory spaces used in format */
    yySpaceCount -= fss->scnSpaceCount;
    if (yySpaceCount < 0) {
	yySpaceCount = 0;
    }
    info->dateStart = p = yyInput;
    info->dateEnd = end;

    /* parse string */
    for (; tok->map != NULL; tok++) {
	map = tok->map;
	/* bypass spaces at begin of input before parsing each token */
	if ( !(opts->flags & CLF_STRICT)
	  && ( map->type != CTOKT_SPACE
	    && map->type != CTOKT_WORD
	    && map->type != CTOKT_CHAR )
	) {
	    while (p < end && isspace(UCHAR(*p))) {
		yySpaceCount--;
		p++;
	    }
	}
	yyInput = p;
	/* end of input string */
	if (p >= end) {
	    break;
	}
	switch (map->type)
	{
	case CTOKT_INT:
	case CTOKT_WIDE:
	if (1) {
	    int minLen, size;
	    int sign = 1;
	    if (map->flags & CLF_SIGNED) {
		if (*p == '+') { yyInput = ++p; }
		else
		if (*p == '-') { yyInput = ++p; sign = -1; };
	    }

	    DetermineGreedySearchLen(opts, info, tok, &minLen, &size);

	    if (size < map->minSize) {
		/* missing input -> error */
		if ((map->flags & CLF_OPTIONAL)) {
		    continue;
		}
		goto not_match;
	    }
	    /* string 2 number, put number into info structure by offset */
	    if (map->offs) {
		p = yyInput; x = p + size;
		if (map->type == CTOKT_INT) {
		    if (size <= 10) {
			_str2int_no((int *)(((char *)info) + map->offs),
				p, x, sign);
		    } else {
			/* we don't have such large scan tokens at the moment */
			goto overflow;
			/* currently unused (maxSize of CTOKT_INT tokens <= 10) */
			#if 0
			if (_str2int((int *)(((char *)info) + map->offs),
				p, x, sign) != TCL_OK) {
			    goto overflow;
			}
			#endif
		    }
		    p = x;
		} else {
		    if (size <= 18) {
			_str2wideInt_no((Tcl_WideInt *)(((char *)info) + map->offs),
				p, x, sign);
		    } else {
			if (_str2wideInt((Tcl_WideInt *)(((char *)info) + map->offs),
				p, x, sign) != TCL_OK) {
			    goto overflow;
			}
		    }
		    p = x;
		}
		flags = (flags & ~map->clearFlags) | map->flags;
	    }
	}
	break;
	case CTOKT_PARSER:
	    switch (map->parser(opts, info, tok)) {
		case TCL_OK:
		break;
		case TCL_RETURN:
		    if ((map->flags & CLF_OPTIONAL)) {
			yyInput = p;
			continue;
		    }
		    goto not_match;
		break;
		default:
		    goto done;
		break;
	    };
	    /* decrement count for possible spaces in match */
	    while (p < yyInput) {
		if (isspace(UCHAR(*p))) {
		    yySpaceCount--;
		}
		p++;
	    }
	    p = yyInput;
	    flags = (flags & ~map->clearFlags) | map->flags;
	break;
	case CTOKT_SPACE:
	    /* at least one space */
	    if (!isspace(UCHAR(*p))) {
		/* unmatched -> error */
		goto not_match;
	    }
	    /* don't decrement yySpaceCount by regular (first expected space), 
	     * already considered above with fss->scnSpaceCount */;
	    p++;
	    while (p < end && isspace(UCHAR(*p))) {
		yySpaceCount--;
		p++;
	    }
	break;
	case CTOKT_WORD:
	    x = FindWordEnd(tok, p, end);
	    if (!x) {
		/* no match -> error */
		goto not_match;
	    }
	    p = x;
	break;
	case CTOKT_CHAR:
	    x = (char *)map->data;
	    if (*x != *p) {
		/* no match -> error */
		goto not_match;
	    }
	    if (isspace(UCHAR(*x))) {
		yySpaceCount--;
	    }
	    p++;
	break;
	}
    }
    /* check end was reached */
    if (p < end) {
	/* in non-strict mode bypass spaces at end of input */
	if ( !(opts->flags & CLF_STRICT) && isspace(UCHAR(*p)) ) {
	    p++;
	    while (p < end && isspace(UCHAR(*p))) {
		p++;
	    }
	}
	/* something after last token - wrong format */
	if (p < end) {
	    goto not_match;
	}
    }
    /* end of string, check only optional tokens at end, otherwise - not match */
    while (tok->map != NULL) {
	if (!(opts->flags & CLF_STRICT) && (tok->map->type == CTOKT_SPACE)) {
	    tok++;
	    if (tok->map == NULL) {
		/* no tokens anymore - trailing spaces are mandatory */
		goto not_match;
	    }
	}
	if (!(tok->map->flags & CLF_OPTIONAL)) {
	    goto not_match;
	}
	tok++;
    }

    /*
     * Invalidate result
     */
    flags |= info->flags;

    /* seconds token (%s) take precedence over all other tokens */
    if ((opts->flags & CLF_EXTENDED) || !(flags & CLF_POSIXSEC)) {
	if (flags & CLF_DATE) {

	    if (!(flags & CLF_JULIANDAY)) {
		info->flags |= CLF_ASSEMBLE_SECONDS|CLF_ASSEMBLE_JULIANDAY;

		/* dd precedence below ddd */
		switch (flags & (CLF_MONTH|CLF_DAYOFYEAR|CLF_DAYOFMONTH)) {
		    case (CLF_DAYOFYEAR|CLF_DAYOFMONTH):
		    /* miss month: ddd over dd (without month) */
		    flags &= ~CLF_DAYOFMONTH;
		    case (CLF_DAYOFYEAR):
		    /* ddd over naked weekday */
		    if (!(flags & CLF_ISO8601YEAR)) {
			flags &= ~CLF_ISO8601WEAK;
		    }
		    break;
		    case (CLF_MONTH|CLF_DAYOFYEAR|CLF_DAYOFMONTH):
		    /* both available: mmdd over ddd */
		    case (CLF_MONTH|CLF_DAYOFMONTH):
		    case (CLF_DAYOFMONTH):
		    /* mmdd / dd over naked weekday */
		    if (!(flags & CLF_ISO8601YEAR)) {
			flags &= ~CLF_ISO8601WEAK;
		    }
		    break;
		    /* neither mmdd nor ddd available */
		    case 0:
		    /* but we have day of the week, which can be used */
		    if (flags & CLF_DAYOFWEEK) {
			/* prefer week based calculation of julianday */
			flags |= CLF_ISO8601WEAK;
		    }
		}

		/* YearWeekDay below YearMonthDay */
		if ( (flags & CLF_ISO8601WEAK)
		  && ( (flags & (CLF_YEAR|CLF_DAYOFYEAR)) == (CLF_YEAR|CLF_DAYOFYEAR)
		    || (flags & (CLF_YEAR|CLF_DAYOFMONTH|CLF_MONTH)) == (CLF_YEAR|CLF_DAYOFMONTH|CLF_MONTH)
		  )
		) {
		    /* yy precedence below yyyy */
		    if (!(flags & CLF_ISO8601CENTURY) && (flags & CLF_CENTURY)) {
			/* normally precedence of ISO is higher, but no century - so put it down */
			flags &= ~CLF_ISO8601WEAK;
		    }
		    else
		    /* yymmdd or yyddd over naked weekday */
		    if (!(flags & CLF_ISO8601YEAR)) {
			flags &= ~CLF_ISO8601WEAK;
		    }
		}

		if ( (flags & CLF_YEAR) ) {
		    if (yyYear < 100) {
			if (!(flags & CLF_CENTURY)) {
			    if (yyYear >= dataPtr->yearOfCenturySwitch) {
				yyYear -= 100;
			    }
			    yyYear += dataPtr->currentYearCentury;
			} else {
			    yyYear += info->dateCentury * 100;
			}
		    }
		} 
		if ( (flags & (CLF_ISO8601WEAK|CLF_ISO8601YEAR)) ) {
		    if ((flags & (CLF_ISO8601YEAR|CLF_YEAR)) == CLF_YEAR) {
		    	/* for calculations expected iso year */
			info->date.iso8601Year = yyYear;
		    }
		    else
		    if (info->date.iso8601Year < 100) {
			if (!(flags & CLF_ISO8601CENTURY)) {
			    if (info->date.iso8601Year >= dataPtr->yearOfCenturySwitch) {
				info->date.iso8601Year -= 100;
			    }
			    info->date.iso8601Year += dataPtr->currentYearCentury;
			} else {
			    info->date.iso8601Year += info->dateCentury * 100;
			}
		    }
		    if ((flags & (CLF_ISO8601YEAR|CLF_YEAR)) == CLF_ISO8601YEAR) {
		    	/* for calculations expected year (e. g. CLF_ISO8601WEAK not set) */
			yyYear = info->date.iso8601Year;
		    }
		}
	    }
	}

	/* if no time - reset time */
	if (!(flags & (CLF_TIME|CLF_LOCALSEC|CLF_POSIXSEC))) {
	    info->flags |= CLF_ASSEMBLE_SECONDS;
	    yydate.localSeconds = 0;
	}

	if (flags & CLF_TIME) {
	    info->flags |= CLF_ASSEMBLE_SECONDS;
	    yySecondOfDay = ToSeconds(yyHour, yyMinutes,
				yySeconds, yyMeridian);
	} else
	if (!(flags & (CLF_LOCALSEC|CLF_POSIXSEC))) {
	    info->flags |= CLF_ASSEMBLE_SECONDS;
	    yySecondOfDay = yydate.localSeconds % SECONDS_PER_DAY;
	}
    }

    /* tell caller which flags were set */
    info->flags |= flags;

    ret = TCL_OK;
    goto done;

overflow:

    Tcl_SetObjResult(opts->interp, Tcl_NewStringObj("requested date too large to represent",
	-1));
    Tcl_SetErrorCode(opts->interp, "CLOCK", "dateTooLarge", NULL);
    goto done;

not_match:

  #if 1
    Tcl_SetObjResult(opts->interp, Tcl_NewStringObj("input string does not match supplied format",
	-1));
  #else
	/* to debug where exactly scan breaks */
    Tcl_SetObjResult(opts->interp, Tcl_ObjPrintf(
	"input string \"%s\" does not match supplied format \"%s\","
	" locale \"%s\" - token \"%s\"",
	info->dateStart, HashEntry4FmtScn(fss)->key.string,
        Tcl_GetString(opts->localeObj), 
        tok && tok->tokWord.start ? tok->tokWord.start : "NULL"));
  #endif
    Tcl_SetErrorCode(opts->interp, "CLOCK", "badInputString", NULL);

done:

    return ret;
}

#define FrmResultIsAllocated(dateFmt) \
    (dateFmt->resEnd - dateFmt->resMem > MIN_FMT_RESULT_BLOCK_ALLOC)

static inline int
FrmResultAllocate(
    register DateFormat *dateFmt,
    int len)
{
    int needed = dateFmt->output + len - dateFmt->resEnd;
    if (needed >= 0) { /* >= 0 - regards NTS zero */
	int newsize = dateFmt->resEnd - dateFmt->resMem
		    + needed + MIN_FMT_RESULT_BLOCK_ALLOC*2;
	char *newRes;
	/* differentiate between stack and memory */
	if (!FrmResultIsAllocated(dateFmt)) {
	    newRes = ckalloc(newsize);
	    if (newRes == NULL) {
		return TCL_ERROR;
	    }
	    memcpy(newRes, dateFmt->resMem, dateFmt->output - dateFmt->resMem);
	} else {
	    newRes = ckrealloc(dateFmt->resMem, newsize);
	    if (newRes == NULL) {
		return TCL_ERROR;
	    }
	}
	dateFmt->output = newRes + (dateFmt->output - dateFmt->resMem);
	dateFmt->resMem = newRes;
	dateFmt->resEnd = newRes + newsize;
    }
    return TCL_OK;
}

static int
ClockFmtToken_HourAMPM_Proc(
    ClockFmtScnCmdArgs *opts,
    DateFormat *dateFmt,
    ClockFormatToken *tok,
    int *val)
{
    *val = ( ( *val + SECONDS_PER_DAY - 3600 ) / 3600 ) % 12 + 1;
    return TCL_OK;
}

static int
ClockFmtToken_AMPM_Proc(
    ClockFmtScnCmdArgs *opts,
    DateFormat *dateFmt,
    ClockFormatToken *tok,
    int *val)
{
    Tcl_Obj *mcObj;
    const char *s;
    int len;

    if (*val < (SECONDS_PER_DAY / 2)) {
	mcObj = ClockMCGet(opts, MCLIT_AM);
    } else {
	mcObj = ClockMCGet(opts, MCLIT_PM);
    }
    if (mcObj == NULL) {
	return TCL_ERROR;
    }
    s = TclGetString(mcObj); len = mcObj->length;
    if (FrmResultAllocate(dateFmt, len) != TCL_OK) { return TCL_ERROR; };
    memcpy(dateFmt->output, s, len + 1);
    if (*tok->tokWord.start == 'p') {
	len = Tcl_UtfToUpper(dateFmt->output);
    }
    dateFmt->output += len;

    return TCL_OK;
}

static int
ClockFmtToken_StarDate_Proc(
    ClockFmtScnCmdArgs *opts,
    DateFormat *dateFmt,
    ClockFormatToken *tok,
    int *val)
 {
    int fractYear;
    /* Get day of year, zero based */
    int v = dateFmt->date.dayOfYear - 1;

    /* Convert day of year to a fractional year */
    if (IsGregorianLeapYear(&dateFmt->date)) {
	fractYear = 1000 * v / 366;
    } else {
	fractYear = 1000 * v / 365;
    }

    /* Put together the StarDate as "Stardate %02d%03d.%1d" */
    if (FrmResultAllocate(dateFmt, 30) != TCL_OK) { return TCL_ERROR; };
    memcpy(dateFmt->output, "Stardate ", 9);
    dateFmt->output += 9;
    dateFmt->output = _itoaw(dateFmt->output,
	dateFmt->date.year - RODDENBERRY, '0', 2);
    dateFmt->output = _itoaw(dateFmt->output,
	fractYear, '0', 3);
    *dateFmt->output++ = '.';
    /* be sure positive after decimal point (note: clock-value can be negative) */
    v = dateFmt->date.secondOfDay / ( SECONDS_PER_DAY / 10 );
    if (v < 0) v = 10 + v;
    dateFmt->output = _itoaw(dateFmt->output, v, '0', 1);

    return TCL_OK;
}
static int
ClockFmtToken_WeekOfYear_Proc(
    ClockFmtScnCmdArgs *opts,
    DateFormat *dateFmt,
    ClockFormatToken *tok,
    int *val)
{
    int dow = dateFmt->date.dayOfWeek;
    if (*tok->tokWord.start == 'U') {
	if (dow == 7) {
	    dow = 0;
	}
	dow++;
    }
    *val = ( dateFmt->date.dayOfYear - dow + 7 ) / 7;
    return TCL_OK;
}
static int
ClockFmtToken_JDN_Proc(
    ClockFmtScnCmdArgs *opts,
    DateFormat *dateFmt,
    ClockFormatToken *tok,
    int *val)
 {
    Tcl_WideInt intJD = dateFmt->date.julianDay;
    int fractJD;

    /* Convert to JDN parts (regarding start offset) and time fraction */
    fractJD = dateFmt->date.secondOfDay 
	- (int)tok->map->offs; /* 0 for calendar or 43200 for astro JD */
    if (fractJD < 0) {
    	intJD--;
	fractJD += SECONDS_PER_DAY;
    }
    if (fractJD && intJD < 0) { /* avoid jump over 0, by negative JD's */
	intJD++;
	if (intJD == 0) {
	    /* -0.0 / -0.9 has zero integer part, so append "-" extra */
	    if (FrmResultAllocate(dateFmt, 1) != TCL_OK) { return TCL_ERROR; };
	    *dateFmt->output++ = '-';
	}
	/* and inverse seconds of day, -0(75) -> -0.25 as float */
	fractJD = SECONDS_PER_DAY - fractJD;
    }

    /* 21 is max width of (negative) wide-int (rather smaller, but anyway a time fraction below) */
    if (FrmResultAllocate(dateFmt, 21) != TCL_OK) { return TCL_ERROR; };
    dateFmt->output = _witoaw(dateFmt->output, intJD, '0', 1);
    /* simplest cases .0 and .5 */
    if (!fractJD || fractJD == (SECONDS_PER_DAY / 2)) {
	/* point + 0 or 5 */
	if (FrmResultAllocate(dateFmt, 1+1) != TCL_OK) { return TCL_ERROR; };
	*dateFmt->output++ = '.';
	*dateFmt->output++ = !fractJD ? '0' : '5';
	*dateFmt->output = '\0';
	return TCL_OK;
    } else {
	/* wrap the time fraction */
	#define JDN_MAX_PRECISION 8
	#define JDN_MAX_PRECBOUND 100000000 /* 10**JDN_MAX_PRECISION */
	char *p;

	/* to float (part after floating point, + 0.5 to round it up) */
	fractJD = (int)(
	    (double)fractJD * JDN_MAX_PRECBOUND / SECONDS_PER_DAY + 0.5
	);
	/* point + integer (as time fraction after floating point) */
	if (FrmResultAllocate(dateFmt, 1+JDN_MAX_PRECISION) != TCL_OK) { return TCL_ERROR; };
	*dateFmt->output++ = '.';
	p = _itoaw(dateFmt->output, fractJD, '0', JDN_MAX_PRECISION);
	/* remove trailing zero's */
	dateFmt->output++;
	while (p > dateFmt->output && *(p-1) == '0') {p--;}
	*p = '\0';
	dateFmt->output = p;
    }
    return TCL_OK;
}
static int
ClockFmtToken_TimeZone_Proc(
    ClockFmtScnCmdArgs *opts,
    DateFormat *dateFmt,
    ClockFormatToken *tok,
    int *val)
{
    if (*tok->tokWord.start == 'z') {
	int z = dateFmt->date.tzOffset;
	char sign = '+';
	if ( z < 0 ) {
	    z = -z;
	    sign = '-';
	}
	if (FrmResultAllocate(dateFmt, 7) != TCL_OK) { return TCL_ERROR; };
	*dateFmt->output++ = sign;
	dateFmt->output = _itoaw(dateFmt->output, z / 3600, '0', 2);
	z %= 3600;
	dateFmt->output = _itoaw(dateFmt->output, z / 60, '0', 2);
	z %= 60;
	if (z != 0) {
	    dateFmt->output = _itoaw(dateFmt->output, z, '0', 2);
	}
    } else {
	Tcl_Obj * objPtr;
	const char *s; int len;
	/* convert seconds to local seconds to obtain tzName object */
	if (ConvertUTCToLocal(opts->clientData, opts->interp,
		&dateFmt->date, opts->timezoneObj,
		GREGORIAN_CHANGE_DATE) != TCL_OK) {
	    return TCL_ERROR;
	};
	objPtr = dateFmt->date.tzName;
	s = TclGetString(objPtr);
	len = objPtr->length;
	if (FrmResultAllocate(dateFmt, len) != TCL_OK) { return TCL_ERROR; };
	memcpy(dateFmt->output, s, len + 1);
	dateFmt->output += len;
    }
    return TCL_OK;
}

static int
ClockFmtToken_LocaleERA_Proc(
    ClockFmtScnCmdArgs *opts,
    DateFormat *dateFmt,
    ClockFormatToken *tok,
    int *val)
{
    Tcl_Obj *mcObj;
    const char *s;
    int len;

    if (dateFmt->date.era == BCE) {
	mcObj = ClockMCGet(opts, MCLIT_BCE);
    } else {
	mcObj = ClockMCGet(opts, MCLIT_CE);
    }
    if (mcObj == NULL) {
	return TCL_ERROR;
    }
    s = TclGetString(mcObj); len = mcObj->length;
    if (FrmResultAllocate(dateFmt, len) != TCL_OK) { return TCL_ERROR; };
    memcpy(dateFmt->output, s, len + 1);
    dateFmt->output += len;

    return TCL_OK;
}

static int
ClockFmtToken_LocaleERAYear_Proc(
    ClockFmtScnCmdArgs *opts,
    DateFormat *dateFmt,
    ClockFormatToken *tok,
    int *val)
{
    int rowc;
    Tcl_Obj **rowv;

    if (dateFmt->localeEra == NULL) {
	Tcl_Obj *mcObj = ClockMCGet(opts, MCLIT_LOCALE_ERAS);
	if (mcObj == NULL) {
	    return TCL_ERROR;
	}
	if (TclListObjGetElements(opts->interp, mcObj, &rowc, &rowv) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (rowc != 0) {
	    dateFmt->localeEra = LookupLastTransition(opts->interp,
		dateFmt->date.localSeconds, rowc, rowv, NULL);
	}
	if (dateFmt->localeEra == NULL) {
	    dateFmt->localeEra = (Tcl_Obj*)1;
	}
    }

    /* if no LOCALE_ERAS in catalog or era not found */
    if (dateFmt->localeEra == (Tcl_Obj*)1) {
	if (FrmResultAllocate(dateFmt, 11) != TCL_OK) { return TCL_ERROR; };
	if (*tok->tokWord.start == 'C') { /* %EC */
	    *val = dateFmt->date.year / 100;
	    dateFmt->output = _itoaw(dateFmt->output,
		*val, '0', 2);
	} else {			  /* %Ey */
	    *val = dateFmt->date.year % 100;
	    dateFmt->output = _itoaw(dateFmt->output,
		*val, '0', 2);
	}
    } else {
	Tcl_Obj *objPtr;
	const char *s;
	int len;
	if (*tok->tokWord.start == 'C') { /* %EC */
	    if (Tcl_ListObjIndex(opts->interp, dateFmt->localeEra, 1,
			&objPtr) != TCL_OK ) {
		return TCL_ERROR;
	    }
	} else {			  /* %Ey */
	    if (Tcl_ListObjIndex(opts->interp, dateFmt->localeEra, 2,
			&objPtr) != TCL_OK ) {
		return TCL_ERROR;
	    }
	    if (Tcl_GetIntFromObj(opts->interp, objPtr, val) != TCL_OK) {
		return TCL_ERROR;
	    }
	    *val = dateFmt->date.year - *val;
	    /* if year in locale numerals */
	    if (*val >= 0 && *val < 100) {
		/* year as integer */
		Tcl_Obj * mcObj = ClockMCGet(opts, MCLIT_LOCALE_NUMERALS);
		if (mcObj == NULL) {
		    return TCL_ERROR;
		}
		if (Tcl_ListObjIndex(opts->interp, mcObj, *val, &objPtr) != TCL_OK) {
		    return TCL_ERROR;
		}
	    } else {
		/* year as integer */
		if (FrmResultAllocate(dateFmt, 11) != TCL_OK) { return TCL_ERROR; };
		dateFmt->output = _itoaw(dateFmt->output,
		    *val, '0', 2);
		return TCL_OK;
	    }
	}
	s = TclGetString(objPtr);
	len = objPtr->length;
	if (FrmResultAllocate(dateFmt, len) != TCL_OK) { return TCL_ERROR; };
	memcpy(dateFmt->output, s, len + 1);
	dateFmt->output += len;
    }
    return TCL_OK;
}


static const char *FmtSTokenMapIndex =
    "demNbByYCHMSIklpaAuwUVzgGjJsntQ";
static ClockFormatTokenMap FmtSTokenMap[] = {
    /* %d */
    {CTOKT_INT, "0", 2, 0, 0, 0, TclOffset(DateFormat, date.dayOfMonth), NULL},
    /* %e */
    {CTOKT_INT, " ", 2, 0, 0, 0, TclOffset(DateFormat, date.dayOfMonth), NULL},
    /* %m */
    {CTOKT_INT, "0", 2, 0, 0, 0, TclOffset(DateFormat, date.month), NULL},
    /* %N */
    {CTOKT_INT, " ", 2, 0, 0, 0, TclOffset(DateFormat, date.month), NULL},
    /* %b %h */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX | CLFMT_DECR, 0, 12, TclOffset(DateFormat, date.month),
	NULL, (void *)MCLIT_MONTHS_ABBREV},
    /* %B */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX | CLFMT_DECR, 0, 12, TclOffset(DateFormat, date.month),
	NULL, (void *)MCLIT_MONTHS_FULL},
    /* %y */
    {CTOKT_INT, "0", 2, 0, 0, 100, TclOffset(DateFormat, date.year), NULL},
    /* %Y */
    {CTOKT_INT, "0", 4, 0, 0, 0, TclOffset(DateFormat, date.year), NULL},
    /* %C */
    {CTOKT_INT, "0", 2, 0, 100, 0, TclOffset(DateFormat, date.year), NULL},
    /* %H */
    {CTOKT_INT, "0", 2, 0, 3600, 24, TclOffset(DateFormat, date.secondOfDay), NULL},
    /* %M */
    {CTOKT_INT, "0", 2, 0, 60, 60, TclOffset(DateFormat, date.secondOfDay), NULL},
    /* %S */
    {CTOKT_INT, "0", 2, 0, 0, 60, TclOffset(DateFormat, date.secondOfDay), NULL},
    /* %I */
    {CTOKT_INT, "0", 2, CLFMT_CALC, 0, 0, TclOffset(DateFormat, date.secondOfDay),
	ClockFmtToken_HourAMPM_Proc, NULL},
    /* %k */
    {CTOKT_INT, " ", 2, 0, 3600, 24, TclOffset(DateFormat, date.secondOfDay), NULL},
    /* %l */
    {CTOKT_INT, " ", 2, CLFMT_CALC, 0, 0, TclOffset(DateFormat, date.secondOfDay),
	ClockFmtToken_HourAMPM_Proc, NULL},
    /* %p %P */
    {CTOKT_INT, NULL, 0, 0, 0, 0, TclOffset(DateFormat, date.secondOfDay),
	ClockFmtToken_AMPM_Proc, NULL},
    /* %a */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX, 0, 7, TclOffset(DateFormat, date.dayOfWeek),
	NULL, (void *)MCLIT_DAYS_OF_WEEK_ABBREV},
    /* %A */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX, 0, 7, TclOffset(DateFormat, date.dayOfWeek),
	NULL, (void *)MCLIT_DAYS_OF_WEEK_FULL},
    /* %u */
    {CTOKT_INT, " ", 1, 0, 0, 0, TclOffset(DateFormat, date.dayOfWeek), NULL},
    /* %w */
    {CTOKT_INT, " ", 1, 0, 0, 7, TclOffset(DateFormat, date.dayOfWeek), NULL},
    /* %U %W */
    {CTOKT_INT, "0", 2, CLFMT_CALC, 0, 0, TclOffset(DateFormat, date.dayOfYear),
	ClockFmtToken_WeekOfYear_Proc, NULL},
    /* %V */
    {CTOKT_INT, "0", 2, 0, 0, 0, TclOffset(DateFormat, date.iso8601Week), NULL},
    /* %z %Z */
    {CFMTT_PROC, NULL, 0, 0, 0, 0, 0,
	ClockFmtToken_TimeZone_Proc, NULL},
    /* %g */
    {CTOKT_INT, "0", 2, 0, 0, 100, TclOffset(DateFormat, date.iso8601Year), NULL},
    /* %G */
    {CTOKT_INT, "0", 4, 0, 0, 0, TclOffset(DateFormat, date.iso8601Year), NULL},
    /* %j */
    {CTOKT_INT, "0", 3, 0, 0, 0, TclOffset(DateFormat, date.dayOfYear), NULL},
    /* %J */
    {CTOKT_WIDE, "0", 7, 0, 0, 0, TclOffset(DateFormat, date.julianDay), NULL},
    /* %s */
    {CTOKT_WIDE, "0", 1, 0, 0, 0, TclOffset(DateFormat, date.seconds), NULL},
    /* %n */
    {CTOKT_CHAR, "\n", 0, 0, 0, 0, 0, NULL},
    /* %t */
    {CTOKT_CHAR, "\t", 0, 0, 0, 0, 0, NULL},
    /* %Q */
    {CFMTT_PROC, NULL, 0, 0, 0, 0, 0,
	ClockFmtToken_StarDate_Proc, NULL},
};
static const char *FmtSTokenMapAliasIndex[2] = {
    "hPWZ",
    "bpUz"
};

static const char *FmtETokenMapIndex =
    "EJjys";
static ClockFormatTokenMap FmtETokenMap[] = {
    /* %EE */
    {CFMTT_PROC, NULL, 0, 0, 0, 0, 0,
	ClockFmtToken_LocaleERA_Proc, NULL},
    /* %EJ */
    {CFMTT_PROC, NULL, 0, 0, 0, 0, 0, /* calendar JDN starts at midnight */
	ClockFmtToken_JDN_Proc, NULL},
    /* %Ej */
    {CFMTT_PROC, NULL, 0, 0, 0, 0, (SECONDS_PER_DAY/2), /* astro JDN starts at noon */
	ClockFmtToken_JDN_Proc, NULL},
    /* %Ey %EC */
    {CTOKT_INT, NULL, 0, 0, 0, 0, TclOffset(DateFormat, date.year),
	ClockFmtToken_LocaleERAYear_Proc, NULL},
    /* %Es */
    {CTOKT_WIDE, "0", 1, 0, 0, 0, TclOffset(DateFormat, date.localSeconds), NULL},
};
static const char *FmtETokenMapAliasIndex[2] = {
    "C",
    "y"
};

static const char *FmtOTokenMapIndex =
    "dmyHIMSuw";
static ClockFormatTokenMap FmtOTokenMap[] = {
    /* %Od %Oe */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX, 0, 100, TclOffset(DateFormat, date.dayOfMonth),
	NULL, (void *)MCLIT_LOCALE_NUMERALS},
    /* %Om */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX, 0, 100, TclOffset(DateFormat, date.month),
	NULL, (void *)MCLIT_LOCALE_NUMERALS},
    /* %Oy */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX, 0, 100, TclOffset(DateFormat, date.year),
	NULL, (void *)MCLIT_LOCALE_NUMERALS},
    /* %OH %Ok */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX, 3600, 24, TclOffset(DateFormat, date.secondOfDay),
	NULL, (void *)MCLIT_LOCALE_NUMERALS},
    /* %OI %Ol */
    {CTOKT_INT, NULL, 0, CLFMT_CALC | CLFMT_LOCALE_INDX, 0, 0, TclOffset(DateFormat, date.secondOfDay),
	ClockFmtToken_HourAMPM_Proc, (void *)MCLIT_LOCALE_NUMERALS},
    /* %OM */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX, 60, 60, TclOffset(DateFormat, date.secondOfDay),
	NULL, (void *)MCLIT_LOCALE_NUMERALS},
    /* %OS */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX, 0, 60, TclOffset(DateFormat, date.secondOfDay),
	NULL, (void *)MCLIT_LOCALE_NUMERALS},
    /* %Ou */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX, 0, 100, TclOffset(DateFormat, date.dayOfWeek),
	NULL, (void *)MCLIT_LOCALE_NUMERALS},
    /* %Ow */
    {CTOKT_INT, NULL, 0, CLFMT_LOCALE_INDX, 0, 7, TclOffset(DateFormat, date.dayOfWeek),
	NULL, (void *)MCLIT_LOCALE_NUMERALS},
};
static const char *FmtOTokenMapAliasIndex[2] = {
    "ekl",
    "dHI"
};

static ClockFormatTokenMap FmtWordTokenMap = {
    CTOKT_WORD, NULL, 0, 0, 0, 0, 0, NULL
};

/*
 *----------------------------------------------------------------------
 */
ClockFmtScnStorage *
ClockGetOrParseFmtFormat(
    Tcl_Interp *interp,		/* Tcl interpreter */
    Tcl_Obj    *formatObj)	/* Format container */
{
    ClockFmtScnStorage *fss;

    fss = Tcl_GetClockFrmScnFromObj(interp, formatObj);
    if (fss == NULL) {
	return NULL;
    }

    /* if format (fmtTok) already tokenized */
    if (fss->fmtTok != NULL) {
	return fss;
    }

    Tcl_MutexLock(&ClockFmtMutex);

    /* first time formatting - tokenize format */
    if (fss->fmtTok == NULL) {
	ClockFormatToken *tok, *fmtTok;
	unsigned int tokCnt;
	register const char *p, *e, *cp;

	e = p = HashEntry4FmtScn(fss)->key.string;
	e += strlen(p);

	/* estimate token count by % char and format length */
	fss->fmtTokC = EstimateTokenCount(p, e);

	fmtTok = tok = ckalloc(sizeof(*tok) * fss->fmtTokC);
	memset(tok, 0, sizeof(*(tok)));
	tokCnt = 1;
	while (p < e) {
	    switch (*p) {
	    case '%':
	    if (1) {
		ClockFormatTokenMap * fmtMap = FmtSTokenMap;
		const char *mapIndex =	FmtSTokenMapIndex,
			  **aliasIndex = FmtSTokenMapAliasIndex;
		if (p+1 >= e) {
		    goto word_tok;
		}
		p++;
		/* try to find modifier: */
		switch (*p) {
		case '%':
		    /* begin new word token - don't join with previous word token,
		     * because current mapping should be "...%%..." -> "...%..." */
		    tok->map = &FmtWordTokenMap;
		    tok->tokWord.start = p;
		    tok->tokWord.end = p+1;
		    AllocTokenInChain(tok, fmtTok, fss->fmtTokC); tokCnt++;
		    p++;
		    continue;
		break;
		case 'E':
		    fmtMap = FmtETokenMap,
		    mapIndex =	FmtETokenMapIndex,
		    aliasIndex = FmtETokenMapAliasIndex;
		    p++;
		break;
		case 'O':
		    fmtMap = FmtOTokenMap,
		    mapIndex = FmtOTokenMapIndex,
		    aliasIndex = FmtOTokenMapAliasIndex;
		    p++;
		break;
		}
		/* search direct index */
		cp = strchr(mapIndex, *p);
		if (!cp || *cp == '\0') {
		    /* search wrapper index (multiple chars for same token) */
		    cp = strchr(aliasIndex[0], *p);
		    if (!cp || *cp == '\0') {
			p--; if (fmtMap != FmtSTokenMap) p--;
			goto word_tok;
		    }
		    cp = strchr(mapIndex, aliasIndex[1][cp - aliasIndex[0]]);
		    if (!cp || *cp == '\0') { /* unexpected, but ... */
		    #ifdef DEBUG
			Tcl_Panic("token \"%c\" has no map in wrapper resolver", *p);
		    #endif
			p--; if (fmtMap != FmtSTokenMap) p--;
			goto word_tok;
		    }
		}
		tok->map = &fmtMap[cp - mapIndex];
		tok->tokWord.start = p;
		/* next token */
		AllocTokenInChain(tok, fmtTok, fss->fmtTokC); tokCnt++;
		p++;
		continue;
	    }
	    break;
	    default:
	      word_tok:
		{
		/* try continue with previous word token */
		ClockFormatToken *wordTok = tok - 1;

		if (wordTok < fmtTok || wordTok->map != &FmtWordTokenMap) {
		    /* start with new word token */
		    wordTok = tok;
		    wordTok->tokWord.start = p;
		    wordTok->map = &FmtWordTokenMap;
		}
		do {
		    p = Tcl_UtfNext(p);
		} while (p < e && *p != '%');
		wordTok->tokWord.end = p;

		if (wordTok == tok) {
		    AllocTokenInChain(tok, fmtTok, fss->fmtTokC); tokCnt++;
		}



		}
	    break;
	    }
	}

	/* correct count of real used tokens and free mem if desired
	 * (1 is acceptable delta to prevent memory fragmentation) */
	if (fss->fmtTokC > tokCnt + (CLOCK_MIN_TOK_CHAIN_BLOCK_SIZE / 2)) {
	    if ( (tok = ckrealloc(fmtTok, tokCnt * sizeof(*tok))) != NULL ) {
		fmtTok = tok;
	    }
	}

	/* now we're ready - assign now to storage (note the threaded race condition) */
	fss->fmtTok = fmtTok;
	fss->fmtTokC = tokCnt;
    }

    Tcl_MutexUnlock(&ClockFmtMutex);
    return fss;
}

/*
 *----------------------------------------------------------------------
 */
int
ClockFormat(
    register DateFormat *dateFmt, /* Date fields used for parsing & converting */
    ClockFmtScnCmdArgs *opts)	  /* Command options */
{
    ClockFmtScnStorage	*fss;
    ClockFormatToken	*tok;
    ClockFormatTokenMap *map;
    char resMem[MIN_FMT_RESULT_BLOCK_ALLOC];

    /* get localized format */
    if (ClockLocalizeFormat(opts) == NULL) {
	return TCL_ERROR;
    }

    if ( !(fss = ClockGetOrParseFmtFormat(opts->interp, opts->formatObj))
      || !(tok = fss->fmtTok)
    ) {
	return TCL_ERROR;
    }

    /* result container object */
    dateFmt->resMem = resMem;
    dateFmt->resEnd = dateFmt->resMem + sizeof(resMem);
    if (fss->fmtMinAlloc > sizeof(resMem)) {
	dateFmt->resMem = ckalloc(fss->fmtMinAlloc);
	dateFmt->resEnd = dateFmt->resMem + fss->fmtMinAlloc;
	if (dateFmt->resMem == NULL) {
	    return TCL_ERROR;
	}
    }
    dateFmt->output = dateFmt->resMem;
    *dateFmt->output = '\0';

    /* do format each token */
    for (; tok->map != NULL; tok++) {
	map = tok->map;
	switch (map->type)
	{
	case CTOKT_INT:
	if (1) {
	    int val = (int)*(int *)(((char *)dateFmt) + map->offs);
	    if (map->fmtproc == NULL) {
		if (map->flags & CLFMT_DECR) {
		    val--;
		}
		if (map->flags & CLFMT_INCR) {
		    val++;
		}
		if (map->divider) {
		    val /= map->divider;
		}
		if (map->divmod) {
		    val %= map->divmod;
		}
	    } else {
		if (map->fmtproc(opts, dateFmt, tok, &val) != TCL_OK) {
		    goto done;
		}
		/* if not calculate only (output inside fmtproc) */
		if (!(map->flags & CLFMT_CALC)) {
		    continue;
		}
	    }
	    if (!(map->flags & CLFMT_LOCALE_INDX)) {
		if (FrmResultAllocate(dateFmt, 11) != TCL_OK) { goto error; };
		if (map->width) {
		    dateFmt->output = _itoaw(dateFmt->output, val, *map->tostr, map->width);
		} else {
		    dateFmt->output += sprintf(dateFmt->output, map->tostr, val);
		}
	    } else {
		const char *s;
		Tcl_Obj * mcObj = ClockMCGet(opts, PTR2INT(map->data) /* mcKey */);
		if (mcObj == NULL) {
		    goto error;
		}
		if ( Tcl_ListObjIndex(opts->interp, mcObj, val, &mcObj) != TCL_OK
		  || mcObj == NULL
		) {
		    goto error;
		}
		s = TclGetString(mcObj);
		if (FrmResultAllocate(dateFmt, mcObj->length) != TCL_OK) { goto error; };
		memcpy(dateFmt->output, s, mcObj->length + 1);
		dateFmt->output += mcObj->length;
	    }
	}
	break;
	case CTOKT_WIDE:
	if (1) {
	    Tcl_WideInt val = *(Tcl_WideInt *)(((char *)dateFmt) + map->offs);
	    if (FrmResultAllocate(dateFmt, 21) != TCL_OK) { goto error; };
	    if (map->width) {
		dateFmt->output = _witoaw(dateFmt->output, val, *map->tostr, map->width);
	    } else {
		dateFmt->output += sprintf(dateFmt->output, map->tostr, val);
	    }
	}
	break;
	case CTOKT_CHAR:
	    if (FrmResultAllocate(dateFmt, 1) != TCL_OK) { goto error; };
	    *dateFmt->output++ = *map->tostr;
	break;
	case CFMTT_PROC:
	    if (map->fmtproc(opts, dateFmt, tok, NULL) != TCL_OK) {
		goto error;
	    };
	break;
	case CTOKT_WORD:
	    if (1) {
		int len = tok->tokWord.end - tok->tokWord.start;
		if (FrmResultAllocate(dateFmt, len) != TCL_OK) { goto error; };
		if (len == 1) {
		    *dateFmt->output++ = *tok->tokWord.start;
		} else {
		    memcpy(dateFmt->output, tok->tokWord.start, len);
		    dateFmt->output += len;
		}
	    }
	break;
	}
    }

    goto done;

error:

    if (dateFmt->resMem != resMem) {
	ckfree(dateFmt->resMem);
    }
    dateFmt->resMem = NULL;

done:

    if (dateFmt->resMem) {
    	size_t size;
	Tcl_Obj * result = Tcl_NewObj();
	result->length = dateFmt->output - dateFmt->resMem;
	size = result->length+1;
	if (dateFmt->resMem == resMem) {
	    result->bytes = ckalloc(size);
	    if (result->bytes == NULL) {
		return TCL_ERROR;
	    }
	    memcpy(result->bytes, dateFmt->resMem, size);
	} else if ((dateFmt->resEnd - dateFmt->resMem) / size > MAX_FMT_RESULT_THRESHOLD) {
	    result->bytes = ckrealloc(dateFmt->resMem, size);
	    if (result->bytes == NULL) {
		result->bytes = dateFmt->resMem;
	    }
	} else {
	    result->bytes = dateFmt->resMem;
	}
	/* save last used buffer length */
	if ( dateFmt->resMem != resMem
	  && fss->fmtMinAlloc < size + MIN_FMT_RESULT_BLOCK_DELTA
	) {
	    fss->fmtMinAlloc = size + MIN_FMT_RESULT_BLOCK_DELTA;
	}
	result->bytes[result->length] = '\0';
	Tcl_SetObjResult(opts->interp, result);
	return TCL_OK;
    }

    return TCL_ERROR;
}


void
ClockFrmScnClearCaches(void)
{
    Tcl_MutexLock(&ClockFmtMutex);
    /* clear caches ... */
    Tcl_MutexUnlock(&ClockFmtMutex);
}

void
ClockFrmScnFinalize()
{
    if (!initialized) {
	return;
    }
    Tcl_MutexLock(&ClockFmtMutex);
#if CLOCK_FMT_SCN_STORAGE_GC_SIZE > 0
    /* clear GC */
    ClockFmtScnStorage_GC.stackPtr = NULL;
    ClockFmtScnStorage_GC.stackBound = NULL;
    ClockFmtScnStorage_GC.count = 0;
#endif
    if (initialized) {
	initialized = 0;
	Tcl_DeleteHashTable(&FmtScnHashTable);
    }
    Tcl_MutexUnlock(&ClockFmtMutex);
    Tcl_MutexFinalize(&ClockFmtMutex);
}
/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
