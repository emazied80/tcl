/*
 * tclListObj.c --
 *
 *	This file contains functions that implement the Tcl list object type.
 *
 * Copyright © 1995-1997 Sun Microsystems, Inc.
 * Copyright © 1998 Scriptics Corporation.
 * Copyright © 2001 Kevin B. Kenny.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tclInt.h"
#include <assert.h>

/*
 * Prototypes for functions defined later in this file:
 */

static List *		AttemptNewList(Tcl_Interp *interp, int objc,
			    Tcl_Obj *const objv[]);
static List *		NewListIntRep(int objc, Tcl_Obj *const objv[], int p);
static void		DupListInternalRep(Tcl_Obj *srcPtr, Tcl_Obj *copyPtr);
static void		FreeListInternalRep(Tcl_Obj *listPtr);
static int		SetListFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr);
static void		UpdateStringOfList(Tcl_Obj *listPtr);

/*
 * The structure below defines the list Tcl object type by means of functions
 * that can be invoked by generic object code.
 *
 * The internal representation of a list object is a two-pointer
 * representation. The first pointer designates a List structure that contains
 * an array of pointers to the element objects, together with integers that
 * represent the current element count and the allocated size of the array.
 * The second pointer is normally NULL; during execution of functions in this
 * file that operate on nested sublists, it is occasionally used as working
 * storage to avoid an auxiliary stack.
 */

const Tcl_ObjType tclListType = {
    "list",			/* name */
    FreeListInternalRep,	/* freeIntRepProc */
    DupListInternalRep,		/* dupIntRepProc */
    UpdateStringOfList,		/* updateStringProc */
    SetListFromAny		/* setFromAnyProc */
};

/* Macros to manipulate the List internal rep */

#define ListSetIntRep(objPtr, listRepPtr)				\
    do {								\
	Tcl_ObjIntRep ir;						\
	ir.twoPtrValue.ptr1 = (listRepPtr);				\
	ir.twoPtrValue.ptr2 = NULL;					\
	(listRepPtr)->refCount++;					\
	Tcl_StoreIntRep((objPtr), &tclListType, &ir);			\
    } while (0)

#define ListGetIntRep(objPtr, listRepPtr)				\
    do {								\
	const Tcl_ObjIntRep *irPtr;					\
	irPtr = TclFetchIntRep((objPtr), &tclListType);		\
	(listRepPtr) = irPtr ? (List *)irPtr->twoPtrValue.ptr1 : NULL;		\
    } while (0)

#define ListResetIntRep(objPtr, listRepPtr) \
    TclFetchIntRep((objPtr), &tclListType)->twoPtrValue.ptr1 = (listRepPtr)

#ifndef TCL_MIN_ELEMENT_GROWTH
#define TCL_MIN_ELEMENT_GROWTH TCL_MIN_GROWTH/sizeof(Tcl_Obj *)
#endif

/*
 *----------------------------------------------------------------------
 *
 * NewListIntRep --
 *
 *	Creates a list internal rep with space for objc elements.  objc
 *	must be > 0.  If objv!=NULL, initializes with the first objc values
 *	in that array.  If objv==NULL, initalize list internal rep to have
 *	0 elements, with space to add objc more.  Flag value "p" indicates
 *	how to behave on failure.
 *
 * Results:
 *	A new List struct with refCount 0 is returned. If some failure
 *	prevents this then if p=0, NULL is returned and otherwise the
 *	routine panics.
 *
 * Side effects:
 *	The ref counts of the elements in objv are incremented since the
 *	resulting list now refers to them.
 *
 *----------------------------------------------------------------------
 */

static List *
NewListIntRep(
    int objc,
    Tcl_Obj *const objv[],
    int p)
{
    List *listRepPtr;

    if (objc <= 0) {
	Tcl_Panic("NewListIntRep: expects postive element count");
    }

    /*
     * First check to see if we'd overflow and try to allocate an object
     * larger than our memory allocator allows. Note that this is actually a
     * fairly small value when you're on a serious 64-bit machine, but that
     * requires API changes to fix. See [Bug 219196] for a discussion.
     */

    if ((size_t)objc > LIST_MAX) {
	if (p) {
	    Tcl_Panic("max length of a Tcl list (%d elements) exceeded",
		    LIST_MAX);
	}
	return NULL;
    }

    listRepPtr = (List *)attemptckalloc(LIST_SIZE(objc));
    if (listRepPtr == NULL) {
	if (p) {
	    Tcl_Panic("list creation failed: unable to alloc %u bytes",
		    LIST_SIZE(objc));
	}
	return NULL;
    }

    listRepPtr->canonicalFlag = 0;
    listRepPtr->refCount = 0;
    listRepPtr->maxElemCount = objc;

    if (objv) {
	Tcl_Obj **elemPtrs;
	int i;

	listRepPtr->elemCount = objc;
	elemPtrs = &listRepPtr->elements;
	for (i = 0;  i < objc;  i++) {
	    elemPtrs[i] = objv[i];
	    Tcl_IncrRefCount(elemPtrs[i]);
	}
    } else {
	listRepPtr->elemCount = 0;
    }
    return listRepPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * AttemptNewList --
 *
 *	Creates a list internal rep with space for objc elements.  objc
 *	must be > 0.  If objv!=NULL, initializes with the first objc values
 *	in that array.  If objv==NULL, initalize list internal rep to have
 *	0 elements, with space to add objc more.
 *
 * Results:
 *	A new List struct with refCount 0 is returned. If some failure
 *	prevents this then NULL is returned, and an error message is left
 *	in the interp result, unless interp is NULL.
 *
 * Side effects:
 *	The ref counts of the elements in objv are incremented since the
 *	resulting list now refers to them.
 *
 *----------------------------------------------------------------------
 */

static List *
AttemptNewList(
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    List *listRepPtr = NewListIntRep(objc, objv, 0);

    if (interp != NULL && listRepPtr == NULL) {
	if (objc > LIST_MAX) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "max length of a Tcl list (%d elements) exceeded",
		    LIST_MAX));
	} else {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "list creation failed: unable to alloc %u bytes",
		    LIST_SIZE(objc)));
	}
	Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
    }
    return listRepPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_NewListObj --
 *
 *	This function is normally called when not debugging: i.e., when
 *	TCL_MEM_DEBUG is not defined. It creates a new list object from an
 *	(objc,objv) array: that is, each of the objc elements of the array
 *	referenced by objv is inserted as an element into a new Tcl object.
 *
 *	When TCL_MEM_DEBUG is defined, this function just returns the result
 *	of calling the debugging version Tcl_DbNewListObj.
 *
 * Results:
 *	A new list object is returned that is initialized from the object
 *	pointers in objv. If objc is less than or equal to zero, an empty
 *	object is returned. The new object's string representation is left
 *	NULL. The resulting new list object has ref count 0.
 *
 * Side effects:
 *	The ref counts of the elements in objv are incremented since the
 *	resulting list now refers to them.
 *
 *----------------------------------------------------------------------
 */

#ifdef TCL_MEM_DEBUG
#undef Tcl_NewListObj

Tcl_Obj *
Tcl_NewListObj(
    int objc,			/* Count of objects referenced by objv. */
    Tcl_Obj *const objv[])	/* An array of pointers to Tcl objects. */
{
    return Tcl_DbNewListObj(objc, objv, "unknown", 0);
}

#else /* if not TCL_MEM_DEBUG */

Tcl_Obj *
Tcl_NewListObj(
    int objc,			/* Count of objects referenced by objv. */
    Tcl_Obj *const objv[])	/* An array of pointers to Tcl objects. */
{
    List *listRepPtr;
    Tcl_Obj *listPtr;

    TclNewObj(listPtr);

    if (objc <= 0) {
	return listPtr;
    }

    /*
     * Create the internal rep.
     */

    listRepPtr = NewListIntRep(objc, objv, 1);

    /*
     * Now create the object.
     */

    TclInvalidateStringRep(listPtr);
    ListSetIntRep(listPtr, listRepPtr);
    return listPtr;
}
#endif /* if TCL_MEM_DEBUG */

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DbNewListObj --
 *
 *	This function is normally called when debugging: i.e., when
 *	TCL_MEM_DEBUG is defined. It creates new list objects. It is the same
 *	as the Tcl_NewListObj function above except that it calls
 *	Tcl_DbCkalloc directly with the file name and line number from its
 *	caller. This simplifies debugging since then the [memory active]
 *	command will report the correct file name and line number when
 *	reporting objects that haven't been freed.
 *
 *	When TCL_MEM_DEBUG is not defined, this function just returns the
 *	result of calling Tcl_NewListObj.
 *
 * Results:
 *	A new list object is returned that is initialized from the object
 *	pointers in objv. If objc is less than or equal to zero, an empty
 *	object is returned. The new object's string representation is left
 *	NULL. The new list object has ref count 0.
 *
 * Side effects:
 *	The ref counts of the elements in objv are incremented since the
 *	resulting list now refers to them.
 *
 *----------------------------------------------------------------------
 */

#ifdef TCL_MEM_DEBUG

Tcl_Obj *
Tcl_DbNewListObj(
    int objc,			/* Count of objects referenced by objv. */
    Tcl_Obj *const objv[],	/* An array of pointers to Tcl objects. */
    const char *file,		/* The name of the source file calling this
				 * function; used for debugging. */
    int line)			/* Line number in the source file; used for
				 * debugging. */
{
    Tcl_Obj *listPtr;
    List *listRepPtr;

    TclDbNewObj(listPtr, file, line);

    if (objc <= 0) {
	return listPtr;
    }

    /*
     * Create the internal rep.
     */

    listRepPtr = NewListIntRep(objc, objv, 1);

    /*
     * Now create the object.
     */

    TclInvalidateStringRep(listPtr);
    ListSetIntRep(listPtr, listRepPtr);

    return listPtr;
}

#else /* if not TCL_MEM_DEBUG */

Tcl_Obj *
Tcl_DbNewListObj(
    int objc,			/* Count of objects referenced by objv. */
    Tcl_Obj *const objv[],	/* An array of pointers to Tcl objects. */
    TCL_UNUSED(const char *) /*file*/,
    TCL_UNUSED(int) /*line*/)
{
    return Tcl_NewListObj(objc, objv);
}
#endif /* TCL_MEM_DEBUG */

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetListObj --
 *
 *	Modify an object to be a list containing each of the objc elements of
 *	the object array referenced by objv.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The object is made a list object and is initialized from the object
 *	pointers in objv. If objc is less than or equal to zero, an empty
 *	object is returned. The new object's string representation is left
 *	NULL. The ref counts of the elements in objv are incremented since the
 *	list now refers to them. The object's old string and internal
 *	representations are freed and its type is set NULL.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetListObj(
    Tcl_Obj *objPtr,		/* Object whose internal rep to init. */
    int objc,			/* Count of objects referenced by objv. */
    Tcl_Obj *const objv[])	/* An array of pointers to Tcl objects. */
{
    List *listRepPtr;

    if (Tcl_IsShared(objPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_SetListObj");
    }

    /*
     * Free any old string rep and any internal rep for the old type.
     */

    TclFreeIntRep(objPtr);
    TclInvalidateStringRep(objPtr);

    /*
     * Set the object's type to "list" and initialize the internal rep.
     * However, if there are no elements to put in the list, just give the
     * object an empty string rep and a NULL type.
     */

    if (objc > 0) {
	listRepPtr = NewListIntRep(objc, objv, 1);
	ListSetIntRep(objPtr, listRepPtr);
    } else {
	Tcl_InitStringRep(objPtr, NULL, 0);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclListObjCopy --
 *
 *	Makes a "pure list" copy of a list value. This provides for the C
 *	level a counterpart of the [lrange $list 0 end] command, while using
 *	internals details to be as efficient as possible.
 *
 * Results:
 *	Normally returns a pointer to a new Tcl_Obj, that contains the same
 *	list value as *listPtr does. The returned Tcl_Obj has a refCount of
 *	zero. If *listPtr does not hold a list, NULL is returned, and if
 *	interp is non-NULL, an error message is recorded there.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclListObjCopy(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr)		/* List object for which an element array is
				 * to be returned. */
{
    Tcl_Obj *copyPtr;
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);
    if (NULL == listRepPtr) {
	if (SetListFromAny(interp, listPtr) != TCL_OK) {
	    return NULL;
	}
    }

    TclNewObj(copyPtr);
    TclInvalidateStringRep(copyPtr);
    DupListInternalRep(listPtr, copyPtr);
    return copyPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclListObjRange --
 *
 *	Makes a slice of a list value.
 *      *listPtr must be known to be a valid list.
 *
 * Results:
 *	Returns a pointer to the sliced list.
 *      This may be a new object or the same object if not shared.
 *
 * Side effects:
 *	The possible conversion of the object referenced by listPtr
 *	to a list object.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclListObjRange(
    Tcl_Obj *listPtr,		/* List object to take a range from. */
    int fromIdx,		/* Index of first element to include. */
    int toIdx)			/* Index of last element to include. */
{
    Tcl_Obj **elemPtrs;
    int listLen, i, newLen;
    List *listRepPtr;

    TclListObjGetElements(NULL, listPtr, &listLen, &elemPtrs);

    if (fromIdx < 0) {
	fromIdx = 0;
    }
    if (toIdx >= listLen) {
	toIdx = listLen-1;
    }
    if (fromIdx > toIdx) {
	Tcl_Obj *obj;
	TclNewObj(obj);
	return obj;
    }

    newLen = toIdx - fromIdx + 1;

    if (Tcl_IsShared(listPtr) ||
	    ((ListRepPtr(listPtr)->refCount > 1))) {
	return Tcl_NewListObj(newLen, &elemPtrs[fromIdx]);
    }

    /*
     * In-place is possible.
     */

    /*
     * Even if nothing below cause any changes, we still want the
     * string-canonizing effect of [lrange 0 end].
     */

    TclInvalidateStringRep(listPtr);

    /*
     * Delete elements that should not be included.
     */

    for (i = 0; i < fromIdx; i++) {
	TclDecrRefCount(elemPtrs[i]);
    }
    for (i = toIdx + 1; i < listLen; i++) {
	TclDecrRefCount(elemPtrs[i]);
    }

    if (fromIdx > 0) {
	memmove(elemPtrs, &elemPtrs[fromIdx],
		(size_t) newLen * sizeof(Tcl_Obj*));
    }

    listRepPtr = ListRepPtr(listPtr);
    listRepPtr->elemCount = newLen;

    return listPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjGetElements --
 *
 *	This function returns an (objc,objv) array of the elements in a list
 *	object.
 *
 * Results:
 *	The return value is normally TCL_OK; in this case *objcPtr is set to
 *	the count of list elements and *objvPtr is set to a pointer to an
 *	array of (*objcPtr) pointers to each list element. If listPtr does not
 *	refer to a list object and the object can not be converted to one,
 *	TCL_ERROR is returned and an error message will be left in the
 *	interpreter's result if interp is not NULL.
 *
 *	The objects referenced by the returned array should be treated as
 *	readonly and their ref counts are _not_ incremented; the caller must
 *	do that if it holds on to a reference. Furthermore, the pointer and
 *	length returned by this function may change as soon as any function is
 *	called on the list object; be careful about retaining the pointer in a
 *	local data structure.
 *
 * Side effects:
 *	The possible conversion of the object referenced by listPtr
 *	to a list object.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjGetElements(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr,	/* List object for which an element array is
				 * to be returned. */
    int *objcPtr,		/* Where to store the count of objects
				 * referenced by objv. */
    Tcl_Obj ***objvPtr)		/* Where to store the pointer to an array of
				 * pointers to the list's objects. */
{
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);

    if (listRepPtr == NULL) {
	int result, length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    *objcPtr = 0;
	    *objvPtr = NULL;
	    return TCL_OK;
	}
	result = SetListFromAny(interp, listPtr);
	if (result != TCL_OK) {
	    return result;
	}
	ListGetIntRep(listPtr, listRepPtr);
    }
    *objcPtr = listRepPtr->elemCount;
    *objvPtr = &listRepPtr->elements;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjAppendList --
 *
 *	This function appends the elements in the list value referenced by
 *	elemListPtr to the list value referenced by listPtr.
 *
 * Results:
 *	The return value is normally TCL_OK. If listPtr or elemListPtr do not
 *	refer to list values, TCL_ERROR is returned and an error message is
 *	left in the interpreter's result if interp is not NULL.
 *
 * Side effects:
 *	The reference counts of the elements in elemListPtr are incremented
 *	since the list now refers to them. listPtr and elemListPtr are
 *	converted, if necessary, to list objects. Also, appending the new
 *	elements may cause listObj's array of element pointers to grow.
 *	listPtr's old string representation, if any, is invalidated.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjAppendList(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr,	/* List object to append elements to. */
    Tcl_Obj *elemListPtr)	/* List obj with elements to append. */
{
    int objc;
    Tcl_Obj **objv;

    if (Tcl_IsShared(listPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_ListObjAppendList");
    }

    /*
     * Pull the elements to append from elemListPtr.
     */

    if (TCL_OK != TclListObjGetElements(interp, elemListPtr, &objc, &objv)) {
	return TCL_ERROR;
    }

    /*
     * Insert the new elements starting after the lists's last element.
     * Delete zero existing elements.
     */

    return Tcl_ListObjReplace(interp, listPtr, LIST_MAX, 0, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjAppendElement --
 *
 *	This function is a special purpose version of Tcl_ListObjAppendList:
 *	it appends a single object referenced by objPtr to the list object
 *	referenced by listPtr. If listPtr is not already a list object, an
 *	attempt will be made to convert it to one.
 *
 * Results:
 *	The return value is normally TCL_OK; in this case objPtr is added to
 *	the end of listPtr's list. If listPtr does not refer to a list object
 *	and the object can not be converted to one, TCL_ERROR is returned and
 *	an error message will be left in the interpreter's result if interp is
 *	not NULL.
 *
 * Side effects:
 *	The ref count of objPtr is incremented since the list now refers to
 *	it. listPtr will be converted, if necessary, to a list object. Also,
 *	appending the new element may cause listObj's array of element
 *	pointers to grow. listPtr's old string representation, if any, is
 *	invalidated.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjAppendElement(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr,		/* List object to append objPtr to. */
    Tcl_Obj *objPtr)		/* Object to append to listPtr's list. */
{
    List *listRepPtr, *newPtr = NULL;
    int numElems, numRequired, needGrow, isShared, attempt;

    if (Tcl_IsShared(listPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_ListObjAppendElement");
    }

    ListGetIntRep(listPtr, listRepPtr);
    if (listRepPtr == NULL) {
	int result, length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    Tcl_SetListObj(listPtr, 1, &objPtr);
	    return TCL_OK;
	}
	result = SetListFromAny(interp, listPtr);
	if (result != TCL_OK) {
	    return result;
	}
	ListGetIntRep(listPtr, listRepPtr);
    }

    numElems = listRepPtr->elemCount;
    numRequired = numElems + 1 ;
    needGrow = (numRequired > listRepPtr->maxElemCount);
    isShared = (listRepPtr->refCount > 1);

    if (numRequired > LIST_MAX) {
	if (interp != NULL) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "max length of a Tcl list (%d elements) exceeded",
		    LIST_MAX));
	    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
	}
	return TCL_ERROR;
    }

    if (needGrow && !isShared) {
	/*
	 * Need to grow + unshared intrep => try to realloc
	 */

	attempt = 2 * numRequired;
	if (attempt <= LIST_MAX) {
	    newPtr = (List *)attemptckrealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr == NULL) {
	    attempt = numRequired + 1 + TCL_MIN_ELEMENT_GROWTH;
	    if (attempt > LIST_MAX) {
		attempt = LIST_MAX;
	    }
	    newPtr = (List *)attemptckrealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr == NULL) {
	    attempt = numRequired;
	    newPtr = (List *)attemptckrealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr) {
	    listRepPtr = newPtr;
	    listRepPtr->maxElemCount = attempt;
	    needGrow = 0;
	}
    }
    if (isShared || needGrow) {
	Tcl_Obj **dst, **src = &listRepPtr->elements;

	/*
	 * Either we have a shared intrep and we must copy to write, or we
	 * need to grow and realloc attempts failed.  Attempt intrep copy.
	 */

	attempt = 2 * numRequired;
	newPtr = AttemptNewList(NULL, attempt, NULL);
	if (newPtr == NULL) {
	    attempt = numRequired + 1 + TCL_MIN_ELEMENT_GROWTH;
	    if (attempt > LIST_MAX) {
		attempt = LIST_MAX;
	    }
	    newPtr = AttemptNewList(NULL, attempt, NULL);
	}
	if (newPtr == NULL) {
	    attempt = numRequired;
	    newPtr = AttemptNewList(interp, attempt, NULL);
	}
	if (newPtr == NULL) {
	    /*
	     * All growth attempts failed; throw the error.
	     */

	    return TCL_ERROR;
	}

	dst = &newPtr->elements;
	newPtr->refCount++;
	newPtr->canonicalFlag = listRepPtr->canonicalFlag;
	newPtr->elemCount = listRepPtr->elemCount;

	if (isShared) {
	    /*
	     * The original intrep must remain undisturbed.  Copy into the new
	     * one and bump refcounts
	     */
	    while (numElems--) {
		*dst = *src++;
		Tcl_IncrRefCount(*dst++);
	    }
	    listRepPtr->refCount--;
	} else {
	    /*
	     * Old intrep to be freed, re-use refCounts.
	     */

	    memcpy(dst, src, numElems * sizeof(Tcl_Obj *));
	    ckfree(listRepPtr);
	}
	listRepPtr = newPtr;
    }
    ListResetIntRep(listPtr, listRepPtr);
    listRepPtr->refCount++;
    TclFreeIntRep(listPtr);
    ListSetIntRep(listPtr, listRepPtr);
    listRepPtr->refCount--;

    /*
     * Add objPtr to the end of listPtr's array of element pointers. Increment
     * the ref count for the (now shared) objPtr.
     */

    *(&listRepPtr->elements + listRepPtr->elemCount) = objPtr;
    Tcl_IncrRefCount(objPtr);
    listRepPtr->elemCount++;

    /*
     * Invalidate any old string representation since the list's internal
     * representation has changed.
     */

    TclInvalidateStringRep(listPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjIndex --
 *
 *	This function returns a pointer to the index'th object from the list
 *	referenced by listPtr. The first element has index 0. If index is
 *	negative or greater than or equal to the number of elements in the
 *	list, a NULL is returned. If listPtr is not a list object, an attempt
 *	will be made to convert it to a list.
 *
 * Results:
 *	The return value is normally TCL_OK; in this case objPtrPtr is set to
 *	the Tcl_Obj pointer for the index'th list element or NULL if index is
 *	out of range. This object should be treated as readonly and its ref
 *	count is _not_ incremented; the caller must do that if it holds on to
 *	the reference. If listPtr does not refer to a list and can't be
 *	converted to one, TCL_ERROR is returned and an error message is left
 *	in the interpreter's result if interp is not NULL.
 *
 * Side effects:
 *	listPtr will be converted, if necessary, to a list object.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjIndex(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr,	/* List object to index into. */
    int index,		/* Index of element to return. */
    Tcl_Obj **objPtrPtr)	/* The resulting Tcl_Obj* is stored here. */
{
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);
    if (listRepPtr == NULL) {
	int result, length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    *objPtrPtr = NULL;
	    return TCL_OK;
	}
	result = SetListFromAny(interp, listPtr);
	if (result != TCL_OK) {
	    return result;
	}
	ListGetIntRep(listPtr, listRepPtr);
    }

    if ((index < 0) || (index >= listRepPtr->elemCount)) {
	*objPtrPtr = NULL;
    } else {
	*objPtrPtr = (&listRepPtr->elements)[index];
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjLength --
 *
 *	This function returns the number of elements in a list object. If the
 *	object is not already a list object, an attempt will be made to
 *	convert it to one.
 *
 * Results:
 *	The return value is normally TCL_OK; in this case *intPtr will be set
 *	to the integer count of list elements. If listPtr does not refer to a
 *	list object and the object can not be converted to one, TCL_ERROR is
 *	returned and an error message will be left in the interpreter's result
 *	if interp is not NULL.
 *
 * Side effects:
 *	The possible conversion of the argument object to a list object.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjLength(
    Tcl_Interp *interp,		/* Used to report errors if not NULL. */
    Tcl_Obj *listPtr,	/* List object whose #elements to return. */
    int *intPtr)	/* The resulting int is stored here. */
{
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);
    if (listRepPtr == NULL) {
	int result, length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    *intPtr = 0;
	    return TCL_OK;
	}
	result = SetListFromAny(interp, listPtr);
	if (result != TCL_OK) {
	    return result;
	}
	ListGetIntRep(listPtr, listRepPtr);
    }

    *intPtr = listRepPtr->elemCount;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ListObjReplace --
 *
 *	This function replaces zero or more elements of the list referenced by
 *	listPtr with the objects from an (objc,objv) array. The objc elements
 *	of the array referenced by objv replace the count elements in listPtr
 *	starting at first.
 *
 *	If the argument first is zero or negative, it refers to the first
 *	element. If first is greater than or equal to the number of elements
 *	in the list, then no elements are deleted; the new elements are
 *	appended to the list. Count gives the number of elements to replace.
 *	If count is zero or negative then no elements are deleted; the new
 *	elements are simply inserted before first.
 *
 *	The argument objv refers to an array of objc pointers to the new
 *	elements to be added to listPtr in place of those that were deleted.
 *	If objv is NULL, no new elements are added. If listPtr is not a list
 *	object, an attempt will be made to convert it to one.
 *
 * Results:
 *	The return value is normally TCL_OK. If listPtr does not refer to a
 *	list object and can not be converted to one, TCL_ERROR is returned and
 *	an error message will be left in the interpreter's result if interp is
 *	not NULL.
 *
 * Side effects:
 *	The ref counts of the objc elements in objv are incremented since the
 *	resulting list now refers to them. Similarly, the ref counts for
 *	replaced objects are decremented. listPtr is converted, if necessary,
 *	to a list object. listPtr's old string representation, if any, is
 *	freed.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ListObjReplace(
    Tcl_Interp *interp,		/* Used for error reporting if not NULL. */
    Tcl_Obj *listPtr,		/* List object whose elements to replace. */
    int first,			/* Index of first element to replace. */
    int count,			/* Number of elements to replace. */
    int objc,			/* Number of objects to insert. */
    Tcl_Obj *const objv[])	/* An array of objc pointers to Tcl objects to
				 * insert. */
{
    List *listRepPtr;
    Tcl_Obj **elemPtrs;
    int needGrow, numElems, numRequired, numAfterLast, start, i, j, isShared;

    if (Tcl_IsShared(listPtr)) {
	Tcl_Panic("%s called with shared object", "Tcl_ListObjReplace");
    }

    ListGetIntRep(listPtr, listRepPtr);
    if (listRepPtr == NULL) {
	int length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    if (objc == 0) {
		return TCL_OK;
	    }
	    Tcl_SetListObj(listPtr, objc, NULL);
	} else {
	    int result = SetListFromAny(interp, listPtr);

	    if (result != TCL_OK) {
		return result;
	    }
	}
	ListGetIntRep(listPtr, listRepPtr);
    }

    /*
     * Note that when count == 0 and objc == 0, this routine is logically a
     * no-op, removing and adding no elements to the list. However, by flowing
     * through this routine anyway, we get the important side effect that the
     * resulting listPtr is a list in canoncial form. This is important.
     * Resist any temptation to optimize this case.
     */

    elemPtrs = &listRepPtr->elements;
    numElems = listRepPtr->elemCount;

    if (first < 0) {
	first = 0;
    }
    if (first >= numElems) {
	first = numElems;	/* So we'll insert after last element. */
    }
    if (count < 0) {
	count = 0;
    } else if (first > INT_MAX - count /* Handle integer overflow */
	    || numElems < first+count) {

	count = numElems - first;
    }

    if (objc > LIST_MAX - (numElems - count)) {
	if (interp != NULL) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "max length of a Tcl list (%d elements) exceeded",
		    LIST_MAX));
	}
	return TCL_ERROR;
    }
    isShared = (listRepPtr->refCount > 1);
    numRequired = numElems - count + objc; /* Known <= LIST_MAX */
    needGrow = numRequired > listRepPtr->maxElemCount;

    for (i = 0;  i < objc;  i++) {
	Tcl_IncrRefCount(objv[i]);
    }

    if (needGrow && !isShared) {
	/* Try to use realloc */
	List *newPtr = NULL;
	int attempt = 2 * numRequired;
	if (attempt <= LIST_MAX) {
	    newPtr = (List *)attemptckrealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr == NULL) {
	    attempt = numRequired + 1 + TCL_MIN_ELEMENT_GROWTH;
	    if (attempt > LIST_MAX) {
		attempt = LIST_MAX;
	    }
	    newPtr = (List *)attemptckrealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr == NULL) {
	    attempt = numRequired;
	    newPtr = (List *)attemptckrealloc(listRepPtr, LIST_SIZE(attempt));
	}
	if (newPtr) {
	    listRepPtr = newPtr;
	    ListResetIntRep(listPtr, listRepPtr);
	    elemPtrs = &listRepPtr->elements;
	    listRepPtr->maxElemCount = attempt;
	    needGrow = numRequired > listRepPtr->maxElemCount;
	}
    }
    if (!needGrow && !isShared) {
	int shift;

	/*
	 * Can use the current List struct. First "delete" count elements
	 * starting at first.
	 */

	for (j = first;  j < first + count;  j++) {
	    Tcl_Obj *victimPtr = elemPtrs[j];

	    TclDecrRefCount(victimPtr);
	}

	/*
	 * Shift the elements after the last one removed to their new
	 * locations.
	 */

	start = first + count;
	numAfterLast = numElems - start;
	shift = objc - count;	/* numNewElems - numDeleted */
	if ((numAfterLast > 0) && (shift != 0)) {
	    Tcl_Obj **src = elemPtrs + start;

	    memmove(src+shift, src, numAfterLast * sizeof(Tcl_Obj*));
	}
    } else {
	/*
	 * Cannot use the current List struct; it is shared, too small, or
	 * both. Allocate a new struct and insert elements into it.
	 */

	List *oldListRepPtr = listRepPtr;
	Tcl_Obj **oldPtrs = elemPtrs;
	int newMax;

	if (needGrow) {
	    newMax = 2 * numRequired;
	} else {
	    newMax = listRepPtr->maxElemCount;
	}

	listRepPtr = AttemptNewList(NULL, newMax, NULL);
	if (listRepPtr == NULL) {
	    unsigned int limit = LIST_MAX - numRequired;
	    unsigned int extra = numRequired - numElems
		    + TCL_MIN_ELEMENT_GROWTH;
	    int growth = (int) ((extra > limit) ? limit : extra);

	    listRepPtr = AttemptNewList(NULL, numRequired + growth, NULL);
	    if (listRepPtr == NULL) {
		listRepPtr = AttemptNewList(interp, numRequired, NULL);
		if (listRepPtr == NULL) {
		    for (i = 0;  i < objc;  i++) {
			/* See bug 3598580 */
#if TCL_MAJOR_VERSION > 8
			Tcl_DecrRefCount(objv[i]);
#else
			objv[i]->refCount--;
#endif
		    }
		    return TCL_ERROR;
		}
	    }
	}

	ListResetIntRep(listPtr, listRepPtr);
	listRepPtr->refCount++;

	elemPtrs = &listRepPtr->elements;

	if (isShared) {
	    /*
	     * The old struct will remain in place; need new refCounts for the
	     * new List struct references. Copy over only the surviving
	     * elements.
	     */

	    for (i=0; i < first; i++) {
		elemPtrs[i] = oldPtrs[i];
		Tcl_IncrRefCount(elemPtrs[i]);
	    }
	    for (i = first + count, j = first + objc;
		    j < numRequired; i++, j++) {
		elemPtrs[j] = oldPtrs[i];
		Tcl_IncrRefCount(elemPtrs[j]);
	    }

	    oldListRepPtr->refCount--;
	} else {
	    /*
	     * The old struct will be removed; use its inherited refCounts.
	     */

	    if (first > 0) {
		memcpy(elemPtrs, oldPtrs, first * sizeof(Tcl_Obj *));
	    }

	    /*
	     * "Delete" count elements starting at first.
	     */

	    for (j = first;  j < first + count;  j++) {
		Tcl_Obj *victimPtr = oldPtrs[j];

		TclDecrRefCount(victimPtr);
	    }

	    /*
	     * Copy the elements after the last one removed, shifted to their
	     * new locations.
	     */

	    start = first + count;
	    numAfterLast = numElems - start;
	    if (numAfterLast > 0) {
		memcpy(elemPtrs + first + objc, oldPtrs + start,
			(size_t) numAfterLast * sizeof(Tcl_Obj *));
	    }

	    ckfree(oldListRepPtr);
	}
    }

    /*
     * Insert the new elements into elemPtrs before "first".
     */

    for (i=0,j=first ; i<objc ; i++,j++) {
	elemPtrs[j] = objv[i];
    }

    /*
     * Update the count of elements.
     */

    listRepPtr->elemCount = numRequired;

    /*
     * Invalidate and free any old representations that may not agree
     * with the revised list's internal representation.
     */

    listRepPtr->refCount++;
    TclFreeIntRep(listPtr);
    ListSetIntRep(listPtr, listRepPtr);
    listRepPtr->refCount--;

    TclInvalidateStringRep(listPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclLindexList --
 *
 *	This procedure handles the 'lindex' command when objc==3.
 *
 * Results:
 *	Returns a pointer to the object extracted, or NULL if an error
 *	occurred. The returned object already includes one reference count for
 *	the pointer returned.
 *
 * Side effects:
 *	None.
 *
 * Notes:
 *	This procedure is implemented entirely as a wrapper around
 *	TclLindexFlat. All it does is reconfigure the argument format into the
 *	form required by TclLindexFlat, while taking care to manage shimmering
 *	in such a way that we tend to keep the most useful intreps and/or
 *	avoid the most expensive conversions.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclLindexList(
    Tcl_Interp *interp,		/* Tcl interpreter. */
    Tcl_Obj *listPtr,		/* List being unpacked. */
    Tcl_Obj *argPtr)		/* Index or index list. */
{

    int index;			/* Index into the list. */
    Tcl_Obj *indexListCopy;
    List *listRepPtr;

    /*
     * Determine whether argPtr designates a list or a single index. We have
     * to be careful about the order of the checks to avoid repeated
     * shimmering; see TIP#22 and TIP#33 for the details.
     */

    ListGetIntRep(argPtr, listRepPtr);
    if ((listRepPtr == NULL)
	    && TclGetIntForIndexM(NULL , argPtr, INT_MAX - 1, &index) == TCL_OK) {
	/*
	 * argPtr designates a single index.
	 */

	return TclLindexFlat(interp, listPtr, 1, &argPtr);
    }

    /*
     * Here we make a private copy of the index list argument to avoid any
     * shimmering issues that might invalidate the indices array below while
     * we are still using it. This is probably unnecessary. It does not appear
     * that any damaging shimmering is possible, and no test has been devised
     * to show any error when this private copy is not made. But it's cheap,
     * and it offers some future-proofing insurance in case the TclLindexFlat
     * implementation changes in some unexpected way, or some new form of
     * trace or callback permits things to happen that the current
     * implementation does not.
     */

    indexListCopy = TclListObjCopy(NULL, argPtr);
    if (indexListCopy == NULL) {
	/*
	 * argPtr designates something that is neither an index nor a
	 * well-formed list. Report the error via TclLindexFlat.
	 */

	return TclLindexFlat(interp, listPtr, 1, &argPtr);
    }

    ListGetIntRep(indexListCopy, listRepPtr);

    assert(listRepPtr != NULL);

    listPtr = TclLindexFlat(interp, listPtr, listRepPtr->elemCount,
		&listRepPtr->elements);
    Tcl_DecrRefCount(indexListCopy);
    return listPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclLindexFlat --
 *
 *	This procedure is the core of the 'lindex' command, with all index
 *	arguments presented as a flat list.
 *
 * Results:
 *	Returns a pointer to the object extracted, or NULL if an error
 *	occurred. The returned object already includes one reference count for
 *	the pointer returned.
 *
 * Side effects:
 *	None.
 *
 * Notes:
 *	The reference count of the returned object includes one reference
 *	corresponding to the pointer returned. Thus, the calling code will
 *	usually do something like:
 *		Tcl_SetObjResult(interp, result);
 *		Tcl_DecrRefCount(result);
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclLindexFlat(
    Tcl_Interp *interp,		/* Tcl interpreter. */
    Tcl_Obj *listPtr,		/* Tcl object representing the list. */
    int indexCount,		/* Count of indices. */
    Tcl_Obj *const indexArray[])/* Array of pointers to Tcl objects that
				 * represent the indices in the list. */
{
    int i;

    Tcl_IncrRefCount(listPtr);

    for (i=0 ; i<indexCount && listPtr ; i++) {
	int index, listLen = 0;
	Tcl_Obj **elemPtrs = NULL, *sublistCopy;

	/*
	 * Here we make a private copy of the current sublist, so we avoid any
	 * shimmering issues that might invalidate the elemPtr array below
	 * while we are still using it. See test lindex-8.4.
	 */

	sublistCopy = TclListObjCopy(interp, listPtr);
	Tcl_DecrRefCount(listPtr);
	listPtr = NULL;

	if (sublistCopy == NULL) {
	    /*
	     * The sublist is not a list at all => error.
	     */

	    break;
	}
	TclListObjGetElements(NULL, sublistCopy, &listLen, &elemPtrs);

	if (TclGetIntForIndexM(interp, indexArray[i], /*endValue*/ listLen-1,
		&index) == TCL_OK) {
	    if (index<0 || index>=listLen) {
		/*
		 * Index is out of range. Break out of loop with empty result.
		 * First check remaining indices for validity
		 */

		while (++i < indexCount) {
		    if (TclGetIntForIndexM(interp, indexArray[i], INT_MAX - 1, &index)
			!= TCL_OK) {
			Tcl_DecrRefCount(sublistCopy);
			return NULL;
		    }
		}
		TclNewObj(listPtr);
	    } else {
		/*
		 * Extract the pointer to the appropriate element.
		 */

		listPtr = elemPtrs[index];
	    }
	    Tcl_IncrRefCount(listPtr);
	}
	Tcl_DecrRefCount(sublistCopy);
    }

    return listPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclLsetList --
 *
 *	Core of the 'lset' command when objc == 4. Objv[2] may be either a
 *	scalar index or a list of indices.
 *      It also handles 'lpop' when given a NULL value.
 *
 * Results:
 *	Returns the new value of the list variable, or NULL if there was an
 *	error. The returned object includes one reference count for the
 *	pointer returned.
 *
 * Side effects:
 *	None.
 *
 * Notes:
 *	This procedure is implemented entirely as a wrapper around
 *	TclLsetFlat. All it does is reconfigure the argument format into the
 *	form required by TclLsetFlat, while taking care to manage shimmering
 *	in such a way that we tend to keep the most useful intreps and/or
 *	avoid the most expensive conversions.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclLsetList(
    Tcl_Interp *interp,		/* Tcl interpreter. */
    Tcl_Obj *listPtr,		/* Pointer to the list being modified. */
    Tcl_Obj *indexArgPtr,	/* Index or index-list arg to 'lset'. */
    Tcl_Obj *valuePtr)		/* Value arg to 'lset' or NULL to 'lpop'. */
{
    int indexCount = 0;		/* Number of indices in the index list. */
    Tcl_Obj **indices = NULL;	/* Vector of indices in the index list. */
    Tcl_Obj *retValuePtr;	/* Pointer to the list to be returned. */
    int index;			/* Current index in the list - discarded. */
    Tcl_Obj *indexListCopy;
    List *listRepPtr;

    /*
     * Determine whether the index arg designates a list or a single index.
     * We have to be careful about the order of the checks to avoid repeated
     * shimmering; see TIP #22 and #23 for details.
     */

    ListGetIntRep(indexArgPtr, listRepPtr);
    if (listRepPtr == NULL
	    && TclGetIntForIndexM(NULL, indexArgPtr, INT_MAX - 1, &index) == TCL_OK) {
	/*
	 * indexArgPtr designates a single index.
	 */

	return TclLsetFlat(interp, listPtr, 1, &indexArgPtr, valuePtr);

    }

    indexListCopy = TclListObjCopy(NULL, indexArgPtr);
    if (indexListCopy == NULL) {
	/*
	 * indexArgPtr designates something that is neither an index nor a
	 * well formed list. Report the error via TclLsetFlat.
	 */

	return TclLsetFlat(interp, listPtr, 1, &indexArgPtr, valuePtr);
    }
    TclListObjGetElements(NULL, indexArgPtr, &indexCount, &indices);

    /*
     * Let TclLsetFlat handle the actual lset'ting.
     */

    retValuePtr = TclLsetFlat(interp, listPtr, indexCount, indices, valuePtr);

    Tcl_DecrRefCount(indexListCopy);
    return retValuePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclLsetFlat --
 *
 *	Core engine of the 'lset' command.
 *      It also handles 'lpop' when given a NULL value.
 *
 * Results:
 *	Returns the new value of the list variable, or NULL if an error
 *	occurred. The returned object includes one reference count for the
 *	pointer returned.
 *
 * Side effects:
 *	On entry, the reference count of the variable value does not reflect
 *	any references held on the stack. The first action of this function is
 *	to determine whether the object is shared, and to duplicate it if it
 *	is. The reference count of the duplicate is incremented. At this
 *	point, the reference count will be 1 for either case, so that the
 *	object will appear to be unshared.
 *
 *	If an error occurs, and the object has been duplicated, the reference
 *	count on the duplicate is decremented so that it is now 0: this
 *	dismisses any memory that was allocated by this function.
 *
 *	If no error occurs, the reference count of the original object is
 *	incremented if the object has not been duplicated, and nothing is done
 *	to a reference count of the duplicate. Now the reference count of an
 *	unduplicated object is 2 (the returned pointer, plus the one stored in
 *	the variable). The reference count of a duplicate object is 1,
 *	reflecting that the returned pointer is the only active reference. The
 *	caller is expected to store the returned value back in the variable
 *	and decrement its reference count. (INST_STORE_* does exactly this.)
 *
 *	Surgery is performed on the unshared list value to produce the result.
 *	TclLsetFlat maintains a linked list of Tcl_Obj's whose string
 *	representations must be spoilt by threading via 'ptr2' of the
 *	two-pointer internal representation. On entry to TclLsetFlat, the
 *	values of 'ptr2' are immaterial; on exit, the 'ptr2' field of any
 *	Tcl_Obj that has been modified is set to NULL.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
TclLsetFlat(
    Tcl_Interp *interp,		/* Tcl interpreter. */
    Tcl_Obj *listPtr,		/* Pointer to the list being modified. */
    int indexCount,		/* Number of index args. */
    Tcl_Obj *const indexArray[],
				/* Index args. */
    Tcl_Obj *valuePtr)		/* Value arg to 'lset' or NULL to 'lpop'. */
{
    int index, result, len;
    Tcl_Obj *subListPtr, *retValuePtr, *chainPtr;
    Tcl_ObjIntRep *irPtr;

    /*
     * If there are no indices, simply return the new value.  (Without
     * indices, [lset] is a synonym for [set].
     * [lpop] does not use this but protect for NULL valuePtr just in case.
     */

    if (indexCount == 0) {
	if (valuePtr != NULL) {
	    Tcl_IncrRefCount(valuePtr);
	}
	return valuePtr;
    }

    /*
     * If the list is shared, make a copy we can modify (copy-on-write).  We
     * use Tcl_DuplicateObj() instead of TclListObjCopy() for a few reasons:
     * 1) we have not yet confirmed listPtr is actually a list; 2) We make a
     * verbatim copy of any existing string rep, and when we combine that with
     * the delayed invalidation of string reps of modified Tcl_Obj's
     * implemented below, the outcome is that any error condition that causes
     * this routine to return NULL, will leave the string rep of listPtr and
     * all elements to be unchanged.
     */

    subListPtr = Tcl_IsShared(listPtr) ? Tcl_DuplicateObj(listPtr) : listPtr;

    /*
     * Anchor the linked list of Tcl_Obj's whose string reps must be
     * invalidated if the operation succeeds.
     */

    retValuePtr = subListPtr;
    chainPtr = NULL;
    result = TCL_OK;

    /*
     * Loop through all the index arguments, and for each one dive into the
     * appropriate sublist.
     */

    do {
	int elemCount;
	Tcl_Obj *parentList, **elemPtrs;

	/*
	 * Check for the possible error conditions...
	 */

	if (TclListObjGetElements(interp, subListPtr, &elemCount, &elemPtrs)
		!= TCL_OK) {
	    /* ...the sublist we're indexing into isn't a list at all. */
	    result = TCL_ERROR;
	    break;
	}

	/*
	 * WARNING: the macro TclGetIntForIndexM is not safe for
	 * post-increments, avoid '*indexArray++' here.
	 */

	if (TclGetIntForIndexM(interp, *indexArray, elemCount - 1, &index)
		!= TCL_OK)  {
	    /* ...the index we're trying to use isn't an index at all. */
	    result = TCL_ERROR;
	    indexArray++;
	    break;
	}
	indexArray++;

	if (index < 0 || index > elemCount
		|| (valuePtr == NULL && index >= elemCount)) {
	    /* ...the index points outside the sublist. */
	    if (interp != NULL) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"index \"%s\" out of range", Tcl_GetString(indexArray[-1])));
		Tcl_SetErrorCode(interp, "TCL", "VALUE", "INDEX"
			"OUTOFRANGE", NULL);
	    }
	    result = TCL_ERROR;
	    break;
	}

	/*
	 * No error conditions.  As long as we're not yet on the last index,
	 * determine the next sublist for the next pass through the loop, and
	 * take steps to make sure it is an unshared copy, as we intend to
	 * modify it.
	 */

	if (--indexCount) {
	    parentList = subListPtr;
	    if (index == elemCount) {
		TclNewObj(subListPtr);
	    } else {
		subListPtr = elemPtrs[index];
	    }
	    if (Tcl_IsShared(subListPtr)) {
		subListPtr = Tcl_DuplicateObj(subListPtr);
	    }

	    /*
	     * Replace the original elemPtr[index] in parentList with a copy
	     * we know to be unshared.  This call will also deal with the
	     * situation where parentList shares its intrep with other
	     * Tcl_Obj's.  Dealing with the shared intrep case can cause
	     * subListPtr to become shared again, so detect that case and make
	     * and store another copy.
	     */

	    if (index == elemCount) {
		Tcl_ListObjAppendElement(NULL, parentList, subListPtr);
	    } else {
		TclListObjSetElement(NULL, parentList, index, subListPtr);
	    }
	    if (Tcl_IsShared(subListPtr)) {
		subListPtr = Tcl_DuplicateObj(subListPtr);
		TclListObjSetElement(NULL, parentList, index, subListPtr);
	    }

	    /*
	     * The TclListObjSetElement() calls do not spoil the string rep of
	     * parentList, and that's fine for now, since all we've done so
	     * far is replace a list element with an unshared copy.  The list
	     * value remains the same, so the string rep. is still valid, and
	     * unchanged, which is good because if this whole routine returns
	     * NULL, we'd like to leave no change to the value of the lset
	     * variable.  Later on, when we set valuePtr in its proper place,
	     * then all containing lists will have their values changed, and
	     * will need their string reps spoiled.  We maintain a list of all
	     * those Tcl_Obj's (via a little intrep surgery) so we can spoil
	     * them at that time.
	     */

	    irPtr = TclFetchIntRep(parentList, &tclListType);
	    irPtr->twoPtrValue.ptr2 = chainPtr;
	    chainPtr = parentList;
	}
    } while (indexCount > 0);

    /*
     * Either we've detected and error condition, and exited the loop with
     * result == TCL_ERROR, or we've successfully reached the last index, and
     * we're ready to store valuePtr.  In either case, we need to clean up our
     * string spoiling list of Tcl_Obj's.
     */

    while (chainPtr) {
	Tcl_Obj *objPtr = chainPtr;
	List *listRepPtr;

	/*
	 * Clear away our intrep surgery mess.
	 */

	irPtr = TclFetchIntRep(objPtr, &tclListType);
	listRepPtr = (List *)irPtr->twoPtrValue.ptr1;
	chainPtr = (Tcl_Obj *)irPtr->twoPtrValue.ptr2;

	if (result == TCL_OK) {

	    /*
	     * We're going to store valuePtr, so spoil string reps of all
	     * containing lists.
	     */

	    listRepPtr->refCount++;
	    TclFreeIntRep(objPtr);
	    ListSetIntRep(objPtr, listRepPtr);
	    listRepPtr->refCount--;

	    TclInvalidateStringRep(objPtr);
	} else {
	    irPtr->twoPtrValue.ptr2 = NULL;
	}
    }

    if (result != TCL_OK) {
	/*
	 * Error return; message is already in interp. Clean up any excess
	 * memory.
	 */

	if (retValuePtr != listPtr) {
	    Tcl_DecrRefCount(retValuePtr);
	}
	return NULL;
    }

    /*
     * Store valuePtr in proper sublist and return. The -1 is to avoid a
     * compiler warning (not a problem because we checked that we have a
     * proper list - or something convertible to one - above).
     */

    len = -1;
    TclListObjLength(NULL, subListPtr, &len);
    if (valuePtr == NULL) {
	Tcl_ListObjReplace(NULL, subListPtr, index, 1, 0, NULL);
    } else if (index == len) {
	Tcl_ListObjAppendElement(NULL, subListPtr, valuePtr);
    } else {
	TclListObjSetElement(NULL, subListPtr, index, valuePtr);
	TclInvalidateStringRep(subListPtr);
    }
    Tcl_IncrRefCount(retValuePtr);
    return retValuePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclListObjSetElement --
 *
 *	Set a single element of a list to a specified value
 *
 * Results:
 *	The return value is normally TCL_OK. If listPtr does not refer to a
 *	list object and cannot be converted to one, TCL_ERROR is returned and
 *	an error message will be left in the interpreter result if interp is
 *	not NULL. Similarly, if index designates an element outside the range
 *	[0..listLength-1], where listLength is the count of elements in the
 *	list object designated by listPtr, TCL_ERROR is returned and an error
 *	message is left in the interpreter result.
 *
 * Side effects:
 *	Tcl_Panic if listPtr designates a shared object. Otherwise, attempts
 *	to convert it to a list with a non-shared internal rep. Decrements the
 *	ref count of the object at the specified index within the list,
 *	replaces with the object designated by valuePtr, and increments the
 *	ref count of the replacement object.
 *
 *	It is the caller's responsibility to invalidate the string
 *	representation of the object.
 *
 *----------------------------------------------------------------------
 */

int
TclListObjSetElement(
    Tcl_Interp *interp,		/* Tcl interpreter; used for error reporting
				 * if not NULL. */
    Tcl_Obj *listPtr,		/* List object in which element should be
				 * stored. */
    int index,			/* Index of element to store. */
    Tcl_Obj *valuePtr)		/* Tcl object to store in the designated list
				 * element. */
{
    List *listRepPtr;		/* Internal representation of the list being
				 * modified. */
    Tcl_Obj **elemPtrs;		/* Pointers to elements of the list. */
    int elemCount;		/* Number of elements in the list. */

    /*
     * Ensure that the listPtr parameter designates an unshared list.
     */

    if (Tcl_IsShared(listPtr)) {
	Tcl_Panic("%s called with shared object", "TclListObjSetElement");
    }

    ListGetIntRep(listPtr, listRepPtr);
    if (listRepPtr == NULL) {
	int result, length;

	(void) Tcl_GetStringFromObj(listPtr, &length);
	if (length == 0) {
	    if (interp != NULL) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"index \"%d\" out of range", index));
		Tcl_SetErrorCode(interp, "TCL", "VALUE", "INDEX",
			"OUTOFRANGE", NULL);
	    }
	    return TCL_ERROR;
	}
	result = SetListFromAny(interp, listPtr);
	if (result != TCL_OK) {
	    return result;
	}
	ListGetIntRep(listPtr, listRepPtr);
    }

    elemCount = listRepPtr->elemCount;

    /*
     * Ensure that the index is in bounds.
     */

    if (index<0 || index>=elemCount) {
	if (interp != NULL) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"index \"%d\" out of range", index));
	    Tcl_SetErrorCode(interp, "TCL", "VALUE", "INDEX",
		    "OUTOFRANGE", NULL);
	}
	return TCL_ERROR;
    }

    /*
     * If the internal rep is shared, replace it with an unshared copy.
     */

    if (listRepPtr->refCount > 1) {
	Tcl_Obj **dst, **src = &listRepPtr->elements;
	List *newPtr = AttemptNewList(NULL, listRepPtr->maxElemCount, NULL);

	if (newPtr == NULL) {
	    newPtr = AttemptNewList(interp, elemCount, NULL);
	    if (newPtr == NULL) {
		return TCL_ERROR;
	    }
	}
	newPtr->refCount++;
	newPtr->elemCount = elemCount;
	newPtr->canonicalFlag = listRepPtr->canonicalFlag;

	dst = &newPtr->elements;
	while (elemCount--) {
	    *dst = *src++;
	    Tcl_IncrRefCount(*dst++);
	}

	listRepPtr->refCount--;

	listRepPtr = newPtr;
	ListResetIntRep(listPtr, listRepPtr);
    }
    elemPtrs = &listRepPtr->elements;

    /*
     * Add a reference to the new list element.
     */

    Tcl_IncrRefCount(valuePtr);

    /*
     * Remove a reference from the old list element.
     */

    Tcl_DecrRefCount(elemPtrs[index]);

    /*
     * Stash the new object in the list.
     */

    elemPtrs[index] = valuePtr;

    /*
     * Invalidate outdated intreps.
     */

    ListGetIntRep(listPtr, listRepPtr);
    listRepPtr->refCount++;
    TclFreeIntRep(listPtr);
    ListSetIntRep(listPtr, listRepPtr);
    listRepPtr->refCount--;

    TclInvalidateStringRep(listPtr);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeListInternalRep --
 *
 *	Deallocate the storage associated with a list object's internal
 *	representation.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees listPtr's List* internal representation, if no longer shared.
 *	May decrement the ref counts of element objects, which may free them.
 *
 *----------------------------------------------------------------------
 */

static void
FreeListInternalRep(
    Tcl_Obj *listPtr)		/* List object with internal rep to free. */
{
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);
    assert(listRepPtr != NULL);

    if (listRepPtr->refCount-- <= 1) {
	Tcl_Obj **elemPtrs = &listRepPtr->elements;
	int i, numElems = listRepPtr->elemCount;

	for (i = 0;  i < numElems;  i++) {
	    Tcl_DecrRefCount(elemPtrs[i]);
	}
	ckfree(listRepPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * DupListInternalRep --
 *
 *	Initialize the internal representation of a list Tcl_Obj to share the
 *	internal representation of an existing list object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The reference count of the List internal rep is incremented.
 *
 *----------------------------------------------------------------------
 */

static void
DupListInternalRep(
    Tcl_Obj *srcPtr,		/* Object with internal rep to copy. */
    Tcl_Obj *copyPtr)		/* Object with internal rep to set. */
{
    List *listRepPtr;

    ListGetIntRep(srcPtr, listRepPtr);
    assert(listRepPtr != NULL);
    ListSetIntRep(copyPtr, listRepPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * SetListFromAny --
 *
 *	Attempt to generate a list internal form for the Tcl object "objPtr".
 *
 * Results:
 *	The return value is TCL_OK or TCL_ERROR. If an error occurs during
 *	conversion, an error message is left in the interpreter's result
 *	unless "interp" is NULL.
 *
 * Side effects:
 *	If no error occurs, a list is stored as "objPtr"s internal
 *	representation.
 *
 *----------------------------------------------------------------------
 */

static int
SetListFromAny(
    Tcl_Interp *interp,		/* Used for error reporting if not NULL. */
    Tcl_Obj *objPtr)		/* The object to convert. */
{
    List *listRepPtr;
    Tcl_Obj **elemPtrs;

    /*
     * Dictionaries are a special case; they have a string representation such
     * that *all* valid dictionaries are valid lists. Hence we can convert
     * more directly. Only do this when there's no existing string rep; if
     * there is, it is the string rep that's authoritative (because it could
     * describe duplicate keys).
     */

    if (!TclHasStringRep(objPtr) && TclHasIntRep(objPtr, &tclDictType)) {
	Tcl_Obj *keyPtr, *valuePtr;
	Tcl_DictSearch search;
	int done, size;

	/*
	 * Create the new list representation. Note that we do not need to do
	 * anything with the string representation as the transformation (and
	 * the reverse back to a dictionary) are both order-preserving. Also
	 * note that since we know we've got a valid dictionary (by
	 * representation) we also know that fetching the size of the
	 * dictionary or iterating over it will not fail.
	 */

	Tcl_DictObjSize(NULL, objPtr, &size);
	listRepPtr = AttemptNewList(interp, size > 0 ? 2*size : 1, NULL);
	if (!listRepPtr) {
	    return TCL_ERROR;
	}
	listRepPtr->elemCount = 2 * size;

	/*
	 * Populate the list representation.
	 */

	elemPtrs = &listRepPtr->elements;
	Tcl_DictObjFirst(NULL, objPtr, &search, &keyPtr, &valuePtr, &done);
	while (!done) {
	    *elemPtrs++ = keyPtr;
	    *elemPtrs++ = valuePtr;
	    Tcl_IncrRefCount(keyPtr);
	    Tcl_IncrRefCount(valuePtr);
	    Tcl_DictObjNext(&search, &keyPtr, &valuePtr, &done);
	}
    } else {
	int estCount, length;
	const char *limit, *nextElem = TclGetStringFromObj(objPtr, &length);

	/*
	 * Allocate enough space to hold a (Tcl_Obj *) for each
	 * (possible) list element.
	 */

	estCount = TclMaxListLength(nextElem, length, &limit);
	estCount += (estCount == 0);	/* Smallest list struct holds 1
					 * element. */
	listRepPtr = AttemptNewList(interp, estCount, NULL);
	if (listRepPtr == NULL) {
	    return TCL_ERROR;
	}
	elemPtrs = &listRepPtr->elements;

	/*
	 * Each iteration, parse and store a list element.
	 */

	while (nextElem < limit) {
	    const char *elemStart;
	    char *check;
	    int elemSize, literal;

	    if (TCL_OK != TclFindElement(interp, nextElem, limit - nextElem,
		    &elemStart, &nextElem, &elemSize, &literal)) {
	    fail:
		while (--elemPtrs >= &listRepPtr->elements) {
		    Tcl_DecrRefCount(*elemPtrs);
		}
		ckfree(listRepPtr);
		return TCL_ERROR;
	    }
	    if (elemStart == limit) {
		break;
	    }

	    TclNewObj(*elemPtrs);
	    TclInvalidateStringRep(*elemPtrs);
	    check = Tcl_InitStringRep(*elemPtrs, literal ? elemStart : NULL,
		    elemSize);
	    if (elemSize && check == NULL) {
		if (interp) {
		    Tcl_SetObjResult(interp, Tcl_NewStringObj(
			    "cannot construct list, out of memory", -1));
		    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
		}
		goto fail;
	    }
	    if (!literal) {
		Tcl_InitStringRep(*elemPtrs, NULL,
			TclCopyAndCollapse(elemSize, elemStart, check));
	    }

	    Tcl_IncrRefCount(*elemPtrs++);/* Since list now holds ref to it. */
	}

 	listRepPtr->elemCount = elemPtrs - &listRepPtr->elements;
    }

    /*
     * Store the new internalRep. We do this as late
     * as possible to allow the conversion code, in particular
     * Tcl_GetStringFromObj, to use the old internalRep.
     */

    ListSetIntRep(objPtr, listRepPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UpdateStringOfList --
 *
 *	Update the string representation for a list object. Note: This
 *	function does not invalidate an existing old string rep so storage
 *	will be lost if this has not already been done.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The object's string is set to a valid string that results from the
 *	list-to-string conversion. This string will be empty if the list has
 *	no elements. The list internal representation should not be NULL and
 *	we assume it is not NULL.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateStringOfList(
    Tcl_Obj *listPtr)		/* List object with string rep to update. */
{
#   define LOCAL_SIZE 64
    char localFlags[LOCAL_SIZE], *flagPtr = NULL;
    int numElems, i, length, bytesNeeded = 0;
    const char *elem, *start;
    char *dst;
    Tcl_Obj **elemPtrs;
    List *listRepPtr;

    ListGetIntRep(listPtr, listRepPtr);

    assert(listRepPtr != NULL);

    numElems = listRepPtr->elemCount;

    /*
     * Mark the list as being canonical; although it will now have a string
     * rep, it is one we derived through proper "canonical" quoting and so
     * it's known to be free from nasties relating to [concat] and [eval].
     */

    listRepPtr->canonicalFlag = 1;

    /*
     * Handle empty list case first, so rest of the routine is simpler.
     */

    if (numElems == 0) {
	Tcl_InitStringRep(listPtr, NULL, 0);
	return;
    }

    /*
     * Pass 1: estimate space, gather flags.
     */

    if (numElems <= LOCAL_SIZE) {
	flagPtr = localFlags;
    } else {
	/*
	 * We know numElems <= LIST_MAX, so this is safe.
	 */

	flagPtr = (char *)ckalloc(numElems);
    }
    elemPtrs = &listRepPtr->elements;
    for (i = 0; i < numElems; i++) {
	flagPtr[i] = (i ? TCL_DONT_QUOTE_HASH : 0);
	elem = TclGetStringFromObj(elemPtrs[i], &length);
	bytesNeeded += TclScanElement(elem, length, flagPtr+i);
	if (bytesNeeded < 0) {
	    Tcl_Panic("max size for a Tcl value (%d bytes) exceeded", INT_MAX);
	}
    }
    if (bytesNeeded > INT_MAX - numElems + 1) {
	Tcl_Panic("max size for a Tcl value (%d bytes) exceeded", INT_MAX);
    }
    bytesNeeded += numElems - 1;

    /*
     * Pass 2: copy into string rep buffer.
     */

    start = dst = Tcl_InitStringRep(listPtr, NULL, bytesNeeded);
    TclOOM(dst, bytesNeeded);
    for (i = 0; i < numElems; i++) {
	flagPtr[i] |= (i ? TCL_DONT_QUOTE_HASH : 0);
	elem = TclGetStringFromObj(elemPtrs[i], &length);
	dst += TclConvertElement(elem, length, dst, flagPtr[i]);
	*dst++ = ' ';
    }

    /* Set the string length to what was actually written, the safe choice */
    (void) Tcl_InitStringRep(listPtr, NULL, dst - 1 - start);

    if (flagPtr != localFlags) {
	ckfree(flagPtr);
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
