/* Copyright 2018 by Michiel de Hoon.  All rights reserved.
 * This file is part of the Biopython distribution and governed by your
 * choice of the "Biopython License Agreement" or the "BSD 3-Clause License".
 * Please see the LICENSE file that should have been included as part of this
 * package.
 */



#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "float.h"


#define HORIZONTAL 0x1
#define VERTICAL 0x2
#define DIAGONAL 0x4
#define STARTPOINT 0x8
#define ENDPOINT 0x10
#define M_MATRIX 0x1
#define Ix_MATRIX 0x2
#define Iy_MATRIX 0x4
#define DONE 0x3
#define NONE 0x7

#define OVERFLOW_ERROR -1
#define MEMORY_ERROR -2

#define CHARINDEX(s) (c = s, c >= 'a' ? c - 'a' : c - 'A')

#define SAFE_ADD(t, s) \
{   if (s != OVERFLOW_ERROR) { \
        term = t; \
        if (term > PY_SSIZE_T_MAX - s) s = OVERFLOW_ERROR; \
        else s += term; \
    } \
}


typedef enum {NeedlemanWunschSmithWaterman,
              Gotoh,
              WatermanSmithBeyer,
              Unknown} Algorithm;

typedef enum {Global, Local} Mode;

typedef struct {
    unsigned char trace : 5;
    unsigned char path : 3;
} Trace;

typedef struct {
    unsigned char Ix : 4;
    unsigned char Iy : 4;
} TraceGapsGotoh;

typedef struct {
    int* MIx;
    int* IyIx;
    int* MIy;
    int* IxIy;
} TraceGapsWatermanSmithBeyer;

static int _convert_single_letter(PyObject* item)
{
    int i;
    char letter = '\0';
    Py_buffer view;
#if PY_MAJOR_VERSION >= 3
    if (PyUnicode_Check(item)) {
        Py_UCS1* data;
        if (PyUnicode_READY(item) == -1) return -1;
        switch (PyUnicode_KIND(item)) {
            case PyUnicode_1BYTE_KIND: break;
            case PyUnicode_2BYTE_KIND:
            case PyUnicode_4BYTE_KIND:
            case PyUnicode_WCHAR_KIND:
                PyErr_SetString(PyExc_ValueError,
                                "expected an ASCII character");
                return -1;
            default:
                PyErr_SetString(PyExc_SystemError,
                                "unknown PyUnicode kind constant");
                return -1;
        }
        data = PyUnicode_1BYTE_DATA(item);
        letter = *((char*)(data));
    }
    else
#endif
    {
        if (!PyObject_CheckBuffer(item)
         || PyObject_GetBuffer(item, &view, PyBUF_FORMAT) == -1) {
            PyErr_SetString(PyExc_ValueError, "expected a single letter");
            return -1;
        }
        if (strcmp(view.format, "B") != 0 || view.len != 1) {
            PyBuffer_Release(&view);
            PyErr_SetString(PyExc_ValueError, "expected a single letter");
            return -1;
        }
        letter = *((char*)(view.buf));
        PyBuffer_Release(&view);
    }
    if (letter >= 'a' && letter <= 'z') i = letter - 'a';
    else if (letter >= 'A' && letter <= 'Z') i = letter - 'A';
    else {
        PyErr_SetString(PyExc_ValueError, "expected an ASCII character");
        return -1;
    }
    return i;
}

static PyObject*
_create_path(Trace** M, int i, int j) {
    PyObject* tuple;
    PyObject* row;
    PyObject* value;
    int path;
    const int ii = i;
    const int jj = j;
    int n = 1;
    int direction = 0;

    while (1) {
        path = M[i][j].path;
        if (!path) break;
        if (path != direction) {
            n++;
            direction = path;
        }
        switch (path) {
            case HORIZONTAL: j++; break;
            case VERTICAL: i++; break;
            case DIAGONAL: i++; j++; break;
        }
    }
    i = ii;
    j = jj;

    direction = 0;
    tuple = PyTuple_New(n);
    if (!tuple) return NULL;
    n = 0;
    while (1) {
        path = M[i][j].path;
        if (path != direction) {
            row = PyTuple_New(2);
            if (!row) break;
#if PY_MAJOR_VERSION >= 3
            value = PyLong_FromLong(i);
#else
            value = PyInt_FromLong(i);
#endif
            if (!value) {
                Py_DECREF(row); /* all references were stolen */
                break;
            }
            PyTuple_SET_ITEM(row, 0, value);
#if PY_MAJOR_VERSION >= 3
            value = PyLong_FromLong(j);
#else
            value = PyInt_FromLong(j);
#endif
            if (!value) {
                Py_DECREF(row); /* all references were stolen */
                break;
            }
            PyTuple_SET_ITEM(row, 1, value);
            PyTuple_SET_ITEM(tuple, n, row);
            n++;
            direction = path;
        }
        switch (path) {
            case HORIZONTAL: j++; break;
            case VERTICAL: i++; break;
            case DIAGONAL: i++; j++; break;
            default: return tuple;
        }
    }
    Py_DECREF(tuple); /* all references were stolen */
    return PyErr_NoMemory();
}

typedef struct {
    PyObject_HEAD
    Trace** M;
    union { TraceGapsGotoh** gotoh;
            TraceGapsWatermanSmithBeyer** waterman_smith_beyer; } gaps;
    int nA;
    int nB;
    int iA;
    int iB;
    Mode mode;
    Algorithm algorithm;
    Py_ssize_t length;
} PathGenerator;

static Py_ssize_t
PathGenerator_needlemanwunsch_length(PathGenerator* self)
{
    int i;
    int j;
    int trace;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    Py_ssize_t term;
    Py_ssize_t count;
    Py_ssize_t temp;
    Py_ssize_t* counts;
    counts = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
    if (!counts) return MEMORY_ERROR;
    counts[0] = 1;
    for (j = 1; j <= nB; j++) {
        trace = M[0][j].trace;
        count = 0;
        if (trace & HORIZONTAL) SAFE_ADD(counts[j-1], count);
        counts[j] = count;
    }
    for (i = 1; i <= nA; i++) {
        trace = M[i][0].trace;
        count = 0;
        if (trace & VERTICAL) SAFE_ADD(counts[0], count);
        temp = counts[0];
        counts[0] = count;
        for (j = 1; j <= nB; j++) {
            trace = M[i][j].trace;
            count = 0;
            if (trace & HORIZONTAL) SAFE_ADD(counts[j-1], count);
            if (trace & VERTICAL) SAFE_ADD(counts[j], count);
            if (trace & DIAGONAL) SAFE_ADD(temp, count);
            temp = counts[j];
            counts[j] = count;
        }
    }
    PyMem_Free(counts);
    return count;
}

static Py_ssize_t
PathGenerator_smithwaterman_length(PathGenerator* self)
{
    int i;
    int j;
    int trace;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    Py_ssize_t term;
    Py_ssize_t count;
    Py_ssize_t total = 0;
    Py_ssize_t temp;
    Py_ssize_t* counts;
    counts = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
    if (!counts) return MEMORY_ERROR;
    counts[0] = 1;
    for (j = 1; j <= nB; j++) counts[j] = 1;
    for (i = 1; i <= nA; i++) {
        temp = counts[0];
        counts[0] = 1;
        for (j = 1; j <= nB; j++) {
            trace = M[i][j].trace;
            count = 0;
            if (trace & DIAGONAL) SAFE_ADD(temp, count);
            if (M[i][j].trace & ENDPOINT) SAFE_ADD(count, total);
            if (trace & HORIZONTAL) SAFE_ADD(counts[j-1], count);
            if (trace & VERTICAL) SAFE_ADD(counts[j], count);
            temp = counts[j];
            if (count == 0 && (trace & STARTPOINT)) count = 1;
            counts[j] = count;
        }
    }
    count = total;
    PyMem_Free(counts);
    return count;
}

static Py_ssize_t
PathGenerator_gotoh_global_length(PathGenerator* self)
{
    int i;
    int j;
    int trace;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    TraceGapsGotoh** gaps = self->gaps.gotoh;
    Py_ssize_t count = MEMORY_ERROR;
    Py_ssize_t term;
    Py_ssize_t M_temp;
    Py_ssize_t Ix_temp;
    Py_ssize_t Iy_temp;
    Py_ssize_t* M_counts = NULL;
    Py_ssize_t* Ix_counts = NULL;
    Py_ssize_t* Iy_counts = NULL;
    M_counts = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
    if (!M_counts) goto exit;
    Ix_counts = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
    if (!Ix_counts) goto exit;
    Iy_counts = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
    if (!Iy_counts) goto exit;
    M_counts[0] = 1;
    Ix_counts[0] = 0;
    Iy_counts[0] = 0;
    for (j = 1; j <= nB; j++) {
        M_counts[j] = 0;
        Ix_counts[j] = 0;
        Iy_counts[j] = 1;
    }
    for (i = 1; i <= nA; i++) {
        M_temp = M_counts[0];
        M_counts[0] = 0;
        Ix_temp = Ix_counts[0];
        Ix_counts[0] = 1;
        Iy_temp = Iy_counts[0];
        Iy_counts[0] = 0;
        for (j = 1; j <= nB; j++) {
            count = 0;
            trace = M[i][j].trace;
            if (trace & M_MATRIX) SAFE_ADD(M_temp, count);
            if (trace & Ix_MATRIX) SAFE_ADD(Ix_temp, count);
            if (trace & Iy_MATRIX) SAFE_ADD(Iy_temp, count);
            M_temp = M_counts[j];
            M_counts[j] = count;
            count = 0;
            trace = gaps[i][j].Ix;
            if (trace & M_MATRIX) SAFE_ADD(M_temp, count);
            if (trace & Ix_MATRIX) SAFE_ADD(Ix_counts[j], count);
            if (trace & Iy_MATRIX) SAFE_ADD(Iy_counts[j], count);
            Ix_temp = Ix_counts[j];
            Ix_counts[j] = count;
            count = 0;
            trace = gaps[i][j].Iy;
            if (trace & M_MATRIX) SAFE_ADD(M_counts[j-1], count);
            if (trace & Ix_MATRIX) SAFE_ADD(Ix_counts[j-1], count);
            if (trace & Iy_MATRIX) SAFE_ADD(Iy_counts[j-1], count);
            Iy_temp = Iy_counts[j];
            Iy_counts[j] = count;
        }
    }
    count = 0;
    if (M[nA][nB].trace) SAFE_ADD(M_counts[nB], count);
    if (gaps[nA][nB].Ix) SAFE_ADD(Ix_counts[nB], count);
    if (gaps[nA][nB].Iy) SAFE_ADD(Iy_counts[nB], count);
exit:
    if (M_counts) PyMem_Free(M_counts);
    if (Ix_counts) PyMem_Free(Ix_counts);
    if (Iy_counts) PyMem_Free(Iy_counts);
    return count;
}

static Py_ssize_t
PathGenerator_gotoh_local_length(PathGenerator* self)
{
    int i;
    int j;
    int trace;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    TraceGapsGotoh** gaps = self->gaps.gotoh;
    Py_ssize_t term;
    Py_ssize_t count = MEMORY_ERROR;
    Py_ssize_t total = 0;
    Py_ssize_t M_temp;
    Py_ssize_t Ix_temp;
    Py_ssize_t Iy_temp;
    Py_ssize_t* M_counts = NULL;
    Py_ssize_t* Ix_counts = NULL;
    Py_ssize_t* Iy_counts = NULL;
    M_counts = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
    if (!M_counts) goto exit;
    Ix_counts = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
    if (!Ix_counts) goto exit;
    Iy_counts = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
    if (!Iy_counts) goto exit;
    M_counts[0] = 1;
    Ix_counts[0] = 0;
    Iy_counts[0] = 0;
    for (j = 1; j <= nB; j++) {
        M_counts[j] = 1;
        Ix_counts[j] = 0;
        Iy_counts[j] = 0;
    }
    for (i = 1; i <= nA; i++) {
        M_temp = M_counts[0];
        M_counts[0] = 1;
        Ix_temp = Ix_counts[0];
        Ix_counts[0] = 0;
        Iy_temp = Iy_counts[0];
        Iy_counts[0] = 0;
        for (j = 1; j <= nB; j++) {
            count = 0;
            trace = M[i][j].trace;
            if (trace & M_MATRIX) SAFE_ADD(M_temp, count);
            if (trace & Ix_MATRIX) SAFE_ADD(Ix_temp, count);
            if (trace & Iy_MATRIX) SAFE_ADD(Iy_temp, count);
            if (count == 0 && (trace & STARTPOINT)) count = 1;
            M_temp = M_counts[j];
            M_counts[j] = count;
            if (M[i][j].trace & ENDPOINT) SAFE_ADD(count, total);
            count = 0;
            trace = gaps[i][j].Ix;
            if (trace & M_MATRIX) SAFE_ADD(M_temp, count);
            if (trace & Ix_MATRIX) SAFE_ADD(Ix_counts[j], count);
            if (trace & Iy_MATRIX) SAFE_ADD(Iy_counts[j], count);
            Ix_temp = Ix_counts[j];
            Ix_counts[j] = count;
            count = 0;
            trace = gaps[i][j].Iy;
            if (trace & M_MATRIX) SAFE_ADD(M_counts[j-1], count);
            if (trace & Ix_MATRIX) SAFE_ADD(Ix_counts[j-1], count);
            if (trace & Iy_MATRIX) SAFE_ADD(Iy_counts[j-1], count);
            Iy_temp = Iy_counts[j];
            Iy_counts[j] = count;
        }
    }
    count = total;
exit:
    if (M_counts) PyMem_Free(M_counts);
    if (Ix_counts) PyMem_Free(Ix_counts);
    if (Iy_counts) PyMem_Free(Iy_counts);
    return count;
}

static Py_ssize_t
PathGenerator_waterman_smith_beyer_global_length(PathGenerator* self)
{
    int i;
    int j;
    int trace;
    int* p;
    int gap;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    TraceGapsWatermanSmithBeyer** gaps = self->gaps.waterman_smith_beyer;
    Py_ssize_t count = MEMORY_ERROR;
    Py_ssize_t term;
    Py_ssize_t** M_count = NULL;
    Py_ssize_t** Ix_count = NULL;
    Py_ssize_t** Iy_count = NULL;
    M_count = PyMem_Malloc((nA+1)*sizeof(Py_ssize_t*));
    if (!M_count) goto exit;
    Ix_count = PyMem_Malloc((nA+1)*sizeof(Py_ssize_t*));
    if (!Ix_count) goto exit;
    Iy_count = PyMem_Malloc((nA+1)*sizeof(Py_ssize_t*));
    if (!Iy_count) goto exit;
    for (i = 0; i <= nA; i++) {
        M_count[i] = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
        if (!M_count[i]) goto exit;
        Ix_count[i] = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
        if (!Ix_count[i]) goto exit;
        Iy_count[i] = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
        if (!Iy_count[i]) goto exit;
    }
    for (i = 0; i <= nA; i++) {
        for (j = 0; j <= nB; j++) {
            count = 0;
            trace = M[i][j].trace;
            if (trace & M_MATRIX) SAFE_ADD(M_count[i-1][j-1], count);
            if (trace & Ix_MATRIX) SAFE_ADD(Ix_count[i-1][j-1], count);
            if (trace & Iy_MATRIX) SAFE_ADD(Iy_count[i-1][j-1], count);
            if (count == 0) count = 1; /* happens at M[0][0] only */
            M_count[i][j] = count;
            count = 0;
            p = gaps[i][j].MIx;
            if (p) {
                while (1) {
                    gap = *p;
                    if (!gap) break;
                    SAFE_ADD(M_count[i-gap][j], count);
                    p++;
                }
            }
            p = gaps[i][j].IyIx;
            if (p) {
                while (1) {
                    gap = *p;
                    if (!gap) break;
                    SAFE_ADD(Iy_count[i-gap][j], count);
                    p++;
                }
            }
            Ix_count[i][j] = count;
            count = 0;
            p = gaps[i][j].MIy;
            if (p) {
                while (1) {
                    gap = *p;
                    if (!gap) break;
                    SAFE_ADD(M_count[i][j-gap], count);
                    p++;
                }
            }
	    p = gaps[i][j].IxIy;
            if (p) {
                while (1) {
                    gap = *p;
                    if (!gap) break;
                    SAFE_ADD(Ix_count[i][j-gap], count);
                    p++;
                }
            }
            Iy_count[i][j] = count;
        }
    }
    count = 0;
    if (M[nA][nB].trace)
        SAFE_ADD(M_count[nA][nB], count);
    if (gaps[nA][nB].MIx[0] || gaps[nA][nB].IyIx[0])
        SAFE_ADD(Ix_count[nA][nB], count);
    if (gaps[nA][nB].MIy[0] || gaps[nA][nB].IxIy[0])
        SAFE_ADD(Iy_count[nA][nB], count);
exit:
    if (M_count) {
        if (Ix_count) {
            if (Iy_count) {
                for (i = 0; i <= nA; i++) {
                    if (!M_count[i]) break;
                    PyMem_Free(M_count[i]);
                    if (!Ix_count[i]) break;
                    PyMem_Free(Ix_count[i]);
                    if (!Iy_count[i]) break;
                    PyMem_Free(Iy_count[i]);
                }
                PyMem_Free(Iy_count);
            }
            PyMem_Free(Ix_count);
        }
        PyMem_Free(M_count);
    }
    return count;
}

static Py_ssize_t
PathGenerator_waterman_smith_beyer_local_length(PathGenerator* self)
{
    int i;
    int j;
    int trace;
    int* p;
    int gap;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    TraceGapsWatermanSmithBeyer** gaps = self->gaps.waterman_smith_beyer;
    Py_ssize_t term;
    Py_ssize_t count = MEMORY_ERROR;
    Py_ssize_t total = 0;
    Py_ssize_t** M_count = NULL;
    Py_ssize_t** Ix_count = NULL;
    Py_ssize_t** Iy_count = NULL;
    M_count = PyMem_Malloc((nA+1)*sizeof(Py_ssize_t*));
    if (!M_count) goto exit;
    Ix_count = PyMem_Malloc((nA+1)*sizeof(Py_ssize_t*));
    if (!Ix_count) goto exit;
    Iy_count = PyMem_Malloc((nA+1)*sizeof(Py_ssize_t*));
    if (!Iy_count) goto exit;
    for (i = 0; i <= nA; i++) {
        M_count[i] = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
        if (!M_count[i]) goto exit;
        Ix_count[i] = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
        if (!Ix_count[i]) goto exit;
        Iy_count[i] = PyMem_Malloc((nB+1)*sizeof(Py_ssize_t));
        if (!Iy_count[i]) goto exit;
    }
    for (i = 0; i <= nA; i++) {
        for (j = 0; j <= nB; j++) {
            count = 0;
            trace = M[i][j].trace;
            if (trace & M_MATRIX) SAFE_ADD(M_count[i-1][j-1], count);
            if (trace & Ix_MATRIX) SAFE_ADD(Ix_count[i-1][j-1], count);
            if (trace & Iy_MATRIX) SAFE_ADD(Iy_count[i-1][j-1], count);
            if (count == 0 && (trace & STARTPOINT)) count = 1;
            M_count[i][j] = count;
            if (M[i][j].trace & ENDPOINT) SAFE_ADD(count, total);
            count = 0;
            p = gaps[i][j].MIx;
            if (p) {
                while (1) {
                    gap = *p;
                    if (!gap) break;
                    SAFE_ADD(M_count[i-gap][j], count);
                    p++;
                }
            }
            p = gaps[i][j].IyIx;
            if (p) {
                while (1) {
                    gap = *p;
                    if (!gap) break;
                    SAFE_ADD(Iy_count[i-gap][j], count);
                    p++;
                }
            }
            Ix_count[i][j] = count;
            count = 0;
            p = gaps[i][j].MIy;
            if (p) {
                while (1) {
                    gap = *p;
                    if (!gap) break;
                    SAFE_ADD(M_count[i][j-gap], count);
                    p++;
                }
            }
            p = gaps[i][j].IxIy;
            if (p) {
                while (1) {
                    gap = *p;
                    if (!gap) break;
                    SAFE_ADD(Ix_count[i][j-gap], count);
                    p++;
                }
            }
            Iy_count[i][j] = count;
        }
    }
    count = total;
exit:
    if (M_count) {
        if (Ix_count) {
            if (Iy_count) {
                for (i = 0; i <= nA; i++) {
                    if (!M_count[i]) break;
                    PyMem_Free(M_count[i]);
                    if (!Ix_count[i]) break;
                    PyMem_Free(Ix_count[i]);
                    if (!Iy_count[i]) break;
                    PyMem_Free(Iy_count[i]);
                }
                PyMem_Free(Iy_count);
            }
            PyMem_Free(Ix_count);
        }
        PyMem_Free(M_count);
    }
    return count;
}

static Py_ssize_t PathGenerator_length(PathGenerator* self) {
    Py_ssize_t length = self->length;
    if (length == 0) {
        switch (self->algorithm) {
            case NeedlemanWunschSmithWaterman:
                switch (self->mode) {
                    case Global:
                        length = PathGenerator_needlemanwunsch_length(self);
                        break;
                    case Local:
                        length = PathGenerator_smithwaterman_length(self);
                        break;
                    default:
                        /* should not happen, but some compilers complain that
                         * that length can be used uninitialized.
                         */
                        PyErr_SetString(PyExc_RuntimeError, "Unknown mode");
                        return -1;
                }
                break;
            case Gotoh:
                switch (self->mode) {
                    case Global:
                        length = PathGenerator_gotoh_global_length(self);
                        break;
                    case Local:
                        length = PathGenerator_gotoh_local_length(self);
                        break;
                    default:
                        /* should not happen, but some compilers complain that
                         * that length can be used uninitialized.
                         */
                        PyErr_SetString(PyExc_RuntimeError, "Unknown mode");
                        return -1;
                }
                break;
            case WatermanSmithBeyer:
                switch (self->mode) {
                    case Global:
                        length = PathGenerator_waterman_smith_beyer_global_length(self);
                        break;
                    case Local:
                        length = PathGenerator_waterman_smith_beyer_local_length(self);
                        break;
                    default:
                        /* should not happen, but some compilers complain that
                         * that length can be used uninitialized.
                         */
                        PyErr_SetString(PyExc_RuntimeError, "Unknown mode");
                        return -1;
                }
                break;
            case Unknown:
            default:
                PyErr_SetString(PyExc_RuntimeError, "Unknown algorithm");
                return -1;
        }
        self->length = length;
    }
    switch (length) {
        case OVERFLOW_ERROR:
            PyErr_Format(PyExc_OverflowError,
                         "number of optimal alignments is larger than %zd",
                         PY_SSIZE_T_MAX);
            break;
        case MEMORY_ERROR:
            PyErr_SetNone(PyExc_MemoryError);
            break;
        default:
            break;
    }
    return length;
}

static void
PathGenerator_dealloc(PathGenerator* self)
{
    int i;
    const int nA = self->nA;
    const Algorithm algorithm = self->algorithm;
    Trace** M = self->M;
    if (M) {
        for (i = 0; i <= nA; i++) {
            if (!M[i]) break;
            PyMem_Free(M[i]);
        }
        PyMem_Free(M);
    }
    switch (algorithm) {
        case NeedlemanWunschSmithWaterman:
            break;
        case Gotoh: {
            TraceGapsGotoh** gaps = self->gaps.gotoh;
            if (gaps) {
                for (i = 0; i <= nA; i++) {
                    if (!gaps[i]) break;
                    PyMem_Free(gaps[i]);
                }
                PyMem_Free(gaps);
            }
            break;
        }
        case WatermanSmithBeyer: {
            TraceGapsWatermanSmithBeyer** gaps = self->gaps.waterman_smith_beyer;
            if (gaps) {
                int j;
                const int nB = self->nB;
                int* trace;
                for (i = 0; i <= nA; i++) {
                    if (!gaps[i]) break;
                    for (j = 0; j <= nB; j++) {
                        trace = gaps[i][j].MIx;
                        if (trace) PyMem_Free(trace);
                        trace = gaps[i][j].IyIx;
                        if (trace) PyMem_Free(trace);
                        trace = gaps[i][j].MIy;
                        if (trace) PyMem_Free(trace);
                        trace = gaps[i][j].IxIy;
                        if (trace) PyMem_Free(trace);
                    }
                    PyMem_Free(gaps[i]);
                }
                PyMem_Free(gaps);
            }
            break;
        }
        case Unknown:
        default:
            PyErr_WriteUnraisable((PyObject*)self);
            break;
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject* PathGenerator_next_needlemanwunsch(PathGenerator* self)
{
    int i = 0;
    int j = 0;
    int path;
    int trace = 0;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;

    path = M[i][j].path;
    if (path == DONE) return NULL;
    if (path == 0) {
        /* Generate the first path. */
        i = nA;
        j = nB;
    }
    else {
        /* We already have a path. Prune the path to see if there are
         * any alternative paths. */
        while (1) {
            if (path == HORIZONTAL) {
                trace = M[i][++j].trace;
                if (trace & VERTICAL) {
                    M[--i][j].path = VERTICAL;
                    break;
                }
                if (trace & DIAGONAL) {
                    M[--i][--j].path = DIAGONAL;
                    break;
                }
            }
            else if (path == VERTICAL) {
                trace = M[++i][j].trace;
                if (trace & DIAGONAL) {
                    M[--i][--j].path = DIAGONAL;
                    break;
                }
            }
            else /* DIAGONAL */ {
                i++;
                j++;
            }
            path = M[i][j].path;
            if (!path) {
                /* we reached the end of the alignment without finding
                 * an alternative path */
                M[0][0].path = DONE;
                return NULL;
            }
        }
    }
    /* Follow the traceback until we reach the origin. */
    while (1) {
        trace = M[i][j].trace;
        if (trace & HORIZONTAL) M[i][--j].path = HORIZONTAL;
        else if (trace & VERTICAL) M[--i][j].path = VERTICAL;
        else if (trace & DIAGONAL) M[--i][--j].path = DIAGONAL;
        else break;
    }
    return _create_path(M, 0, 0);
}

static PyObject* PathGenerator_next_smithwaterman(PathGenerator* self)
{
    int trace = 0;
    int i = self->iA;
    int j = self->iB;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    int path = M[0][0].path;

    if (path == DONE || path == NONE) return NULL;

    path = M[i][j].path;
    if (path) {
        /* We already have a path. Prune the path to see if there are
         * any alternative paths. */
        while (1) {
            if (path == HORIZONTAL) {
                trace = M[i][++j].trace;
                if (trace & VERTICAL) {
                    M[--i][j].path = VERTICAL;
                    break;
                }
                else if (trace & DIAGONAL) {
                    M[--i][--j].path = DIAGONAL;
                    break;
                }
            }
            else if (path == VERTICAL) {
                trace = M[++i][j].trace;
                if (trace & DIAGONAL) {
                    M[--i][--j].path = DIAGONAL;
                    break;
                }
            }
            else /* DIAGONAL */ {
                i++;
                j++;
            }
            path = M[i][j].path;
            if (!path) break;
        }
    }

    if (path) {
        trace = M[i][j].trace;
    } else {
        /* Find a suitable end point for a path.
         * Only allow end points ending at the M matrix. */
        while (1) {
            if (j < nB) j++;
            else if (i < nA) {
                i++;
                j = 0;
            }
            else {
                /* we reached the end of the sequences without finding
                 * an alternative path */
                M[0][0].path = DONE;
                return NULL;
            }
            trace = M[i][j].trace;
            if (trace & ENDPOINT) {
                trace &= DIAGONAL; /* exclude paths ending in a gap */
                break;
            }
        }
        M[i][j].path = 0;
    }

    /* Follow the traceback until we reach the origin. */
    while (1) {
        if (trace & HORIZONTAL) M[i][--j].path = HORIZONTAL;
        else if (trace & VERTICAL) M[--i][j].path = VERTICAL;
        else if (trace & DIAGONAL) M[--i][--j].path = DIAGONAL;
        else if (trace & STARTPOINT) {
            self->iA = i;
            self->iB = j;
            return _create_path(M, i, j);
        }
        else {
            PyErr_SetString(PyExc_RuntimeError,
                "Unexpected trace in PathGenerator_next_smithwaterman");
            return NULL;
        }
        trace = M[i][j].trace;
    }
}

static PyObject* PathGenerator_next_gotoh_global(PathGenerator* self)
{
    int i = 0;
    int j = 0;
    int m;
    int path;
    int trace = 0;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    TraceGapsGotoh** gaps = self->gaps.gotoh;

    m = M_MATRIX;
    path = M[i][j].path;
    if (path == DONE) return NULL;
    if (path == 0) {
        i = nA;
        j = nB;
    }
    else {
        /* We already have a path. Prune the path to see if there are
         * any alternative paths. */
        while (1) {
            path = M[i][j].path;
            if (path == 0) {
                switch (m) {
                    case M_MATRIX: m = Ix_MATRIX; break;
                    case Ix_MATRIX: m = Iy_MATRIX; break;
                    case Iy_MATRIX: m = 0; break;
                }
                break;
            }
            switch (path) {
                case HORIZONTAL: trace = gaps[i][++j].Iy; break;
                case VERTICAL: trace = gaps[++i][j].Ix; break;
                case DIAGONAL: trace = M[++i][++j].trace; break;
            }
            switch (m) {
                case M_MATRIX:
                    if (trace & Ix_MATRIX) {
                        m = Ix_MATRIX;
                        break;
                    }
                case Ix_MATRIX:
                    if (trace & Iy_MATRIX) {
                        m = Iy_MATRIX;
                        break;
                    }
                case Iy_MATRIX:
                default:
                    switch (path) {
                        case HORIZONTAL: m = Iy_MATRIX; break;
                        case VERTICAL: m = Ix_MATRIX; break;
                        case DIAGONAL: m = M_MATRIX; break;
                    }
                    continue;
            }
            switch (path) {
                case HORIZONTAL: j--; break;
                case VERTICAL: i--; break;
                case DIAGONAL: i--; j--; break;
            }
            M[i][j].path = path;
            break;
        }
    }

    if (path == 0) {
        /* Generate a new path. */
        switch (m) {
            case M_MATRIX:
                if (M[nA][nB].trace) {
                   /* m = M_MATRIX; */
                   break;
                }
            case Ix_MATRIX:
                if (gaps[nA][nB].Ix) {
                   m = Ix_MATRIX;
                   break;
                }
            case Iy_MATRIX:
                if (gaps[nA][nB].Iy) {
                   m = Iy_MATRIX;
                   break;
                }
            default:
                /* exhausted this generator */
                M[0][0].path = DONE;
                return NULL;
        }
    }

    switch (m) {
        case M_MATRIX:
            trace = M[i][j].trace;
            path = DIAGONAL;
            i--; j--;
            break;
        case Ix_MATRIX:
            trace = gaps[i][j].Ix;
            path = VERTICAL;
            i--;
            break;
        case Iy_MATRIX:
            trace = gaps[i][j].Iy;
            path = HORIZONTAL;
            j--;
            break;
    }

    while (1) {
        if (trace & M_MATRIX) {
            trace = M[i][j].trace;
            M[i][j].path = path;
            path = DIAGONAL;
            i--; j--;
        }
        else if (trace & Ix_MATRIX) {
            M[i][j].path = path;
            trace = gaps[i][j].Ix;
            path = VERTICAL;
            i--;
        }
        else if (trace & Iy_MATRIX) {
            M[i][j].path = path;
            trace = gaps[i][j].Iy;
            path = HORIZONTAL;
            j--;
        }
        else break;
    }
    return _create_path(M, 0, 0);
}

static PyObject* PathGenerator_next_gotoh_local(PathGenerator* self)
{
    int trace = 0;
    int i;
    int j;
    int m = M_MATRIX;
    int iA = self->iA;
    int iB = self->iB;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    TraceGapsGotoh** gaps = self->gaps.gotoh;
    int path = M[0][0].path;

    if (path == DONE) return NULL;

    path = M[iA][iB].path;

    if (path) {
        i = iA;
        j = iB;
        while (1) {
            /* We already have a path. Prune the path to see if there are
             * any alternative paths. */
            path = M[i][j].path;
            if (path == 0) {
                m = M_MATRIX;
                iA = i;
                iB = j;
                break;
            }
            switch (path) {
                case HORIZONTAL: trace = gaps[i][++j].Iy; break;
                case VERTICAL: trace = gaps[++i][j].Ix; break;
                case DIAGONAL: trace = M[++i][++j].trace; break;
            }
            switch (m) {
                case M_MATRIX:
                    if (trace & Ix_MATRIX) {
                        m = Ix_MATRIX;
                        break;
                    }
                case Ix_MATRIX:
                    if (trace & Iy_MATRIX) {
                        m = Iy_MATRIX;
                        break;
                    }
                case Iy_MATRIX:
                default:
                    switch (path) {
                        case HORIZONTAL: m = Iy_MATRIX; break;
                        case VERTICAL: m = Ix_MATRIX; break;
                        case DIAGONAL: m = M_MATRIX; break;
                    }
                    continue;
            }
            switch (path) {
                case HORIZONTAL: j--; break;
                case VERTICAL: i--; break;
                case DIAGONAL: i--; j--; break;
            }
            M[i][j].path = path;
            break;
        }
    }

    if (path == 0) {
        /* Find the end point for a new path. */
        while (1) {
            if (iB < nB) iB++;
            else if (iA < nA) {
                iA++;
                iB = 0;
            }
            else {
                /* we reached the end of the alignment without finding
                 * an alternative path */
                M[0][0].path = DONE;
                return NULL;
            }
            if (M[iA][iB].trace & ENDPOINT) {
                M[iA][iB].path = 0;
                break;
            }
        }
        m = M_MATRIX;
        i = iA;
        j = iB;
    }

    while (1) {
        switch (m) {
            case M_MATRIX: trace = M[i][j].trace; break;
            case Ix_MATRIX: trace = gaps[i][j].Ix; break;
            case Iy_MATRIX: trace = gaps[i][j].Iy; break;
        }
        if (trace == STARTPOINT) {
            self->iA = i;
            self->iB = j;
            return _create_path(M, i, j);
        }
        switch (m) {
            case M_MATRIX:
                path = DIAGONAL;
                i--;
                j--;
                break;
            case Ix_MATRIX:
                path = VERTICAL;
                i--;
                break;
            case Iy_MATRIX:
                path = HORIZONTAL;
                j--;
                break;
        }
        if (trace & M_MATRIX) m = M_MATRIX;
        else if (trace & Ix_MATRIX) m = Ix_MATRIX;
        else if (trace & Iy_MATRIX) m = Iy_MATRIX;
        else {
            PyErr_SetString(PyExc_RuntimeError,
                "Unexpected trace in PathGenerator_next_gotoh_local");
            return NULL;
        }
        M[i][j].path = path;
    }
    return NULL;
}

static PyObject*
PathGenerator_next_waterman_smith_beyer_global(PathGenerator* self)
{
    int i = 0, j = 0;
    int iA, iB;
    int trace;
    int* gapM;
    int* gapXY;

    int m = M_MATRIX;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    TraceGapsWatermanSmithBeyer** gaps = self->gaps.waterman_smith_beyer;

    int gap;
    int path = M[0][0].path;

    if (path == DONE) return NULL;

    if (path) {
        /* We already have a path. Prune the path to see if there are
         * any alternative paths. */
        while (1) {
            if (!path) {
                m <<= 1;
                break;
            }
            switch (path) {
                case HORIZONTAL:
                    iA = i;
                    iB = j;
                    while (M[i][iB].path == HORIZONTAL) iB++;
                    break;
                case VERTICAL:
                    iA = i;
                    while (M[iA][j].path == VERTICAL) iA++;
                    iB = j;
                    break;
                case DIAGONAL:
                    iA = i + 1;
                    iB = j + 1;
                    break;
                default:
                    PyErr_SetString(PyExc_RuntimeError,
                        "Unexpected path in PathGenerator_next_waterman_smith_beyer_global");
                    return NULL;
            }
            if (i == iA) { /* HORIZONTAL */
                gapM = gaps[iA][iB].MIy;
                gapXY = gaps[iA][iB].IxIy;
                if (m == M_MATRIX) {
                    gap = iB - j;
                    while (*gapM != gap) gapM++;
                    gapM++;
                    gap = *gapM;
                    if (gap) {
                        j = iB - gap;
                        while (j < iB) M[i][--iB].path = HORIZONTAL;
                        break;
                    }
                } else if (m == Ix_MATRIX) {
                    gap = iB - j;
                    while (*gapXY != gap) gapXY++;
                    gapXY++;
                }
                gap = *gapXY;
                if (gap) {
                    m = Ix_MATRIX;
                    j = iB - gap;
                    while (j < iB) M[i][--iB].path = HORIZONTAL;
                    break;
                }
                /* no alternative found; continue pruning */
                m = Iy_MATRIX;
                j = iB;
            }
            else if (j == iB) { /* VERTICAL */
                gapM = gaps[iA][iB].MIx;
                gapXY = gaps[iA][iB].IyIx;
                if (m == M_MATRIX) {
                    gap = iA - i;
                    while (*gapM != gap) gapM++;
                    gapM++;
                    gap = *gapM;
                    if (gap) {
                        i = iA - gap;
                        while (i < iA) M[--iA][j].path = VERTICAL;
                        break;
                    }
                } else if (m == Iy_MATRIX) {
                    gap = iA - i;
                    while (*gapXY != gap) gapXY++;
                    gapXY++;
                }
                gap = *gapXY;
                if (gap) {
                    m = Iy_MATRIX;
                    i = iA - gap;
                    while (i < iA) M[--iA][j].path = VERTICAL;
                    break;
                }
                /* no alternative found; continue pruning */
                m = Ix_MATRIX;
                i = iA;
            }
            else { /* DIAGONAL */
                i = iA - 1;
                j = iB - 1;
                trace = M[iA][iB].trace;
                switch (m) {
                    case M_MATRIX:
                        if (trace & Ix_MATRIX) {
                            m = Ix_MATRIX;
                            M[i][j].path = DIAGONAL;
                            break;
                        }
                    case Ix_MATRIX:
                        if (trace & Iy_MATRIX) {
                            m = Iy_MATRIX;
                            M[i][j].path = DIAGONAL;
                            break;
                        }
                    case Iy_MATRIX:
                    default:
                        /* no alternative found; continue pruning */
                        m = M_MATRIX;
                        i = iA;
                        j = iB;
                        path = M[i][j].path;
                        continue;
                }
                /* alternative found; build path until starting point */
                break;
            }
            path = M[i][j].path;
        }
    }

    if (!path) {
        /* Find a suitable end point for a path. */
        switch (m) {
            case M_MATRIX:
                if (M[nA][nB].trace) {
                    /* m = M_MATRIX; */
                    break;
                }
            case Ix_MATRIX:
                if (gaps[nA][nB].MIx[0] || gaps[nA][nB].IyIx[0]) {
                    m = Ix_MATRIX;
                    break;
                }
            case Iy_MATRIX:
                if (gaps[nA][nB].MIy[0] || gaps[nA][nB].IxIy[0]) {
                    m = Iy_MATRIX;
                    break;
                }
            default:
                M[0][0].path = DONE;
                return NULL;
        }
        i = nA;
        j = nB;
    }

    /* Follow the traceback until we reach the origin. */
    while (1) {
        switch (m) {
            case M_MATRIX:
                trace = M[i][j].trace;
                if (trace & M_MATRIX) m = M_MATRIX;
                else if (trace & Ix_MATRIX) m = Ix_MATRIX;
                else if (trace & Iy_MATRIX) m = Iy_MATRIX;
                else return _create_path(M, i, j);
                i--;
                j--;
                M[i][j].path = DIAGONAL;
                break;
            case Ix_MATRIX:
                gap = gaps[i][j].MIx[0];
                if (gap) m = M_MATRIX;
                else {
                    gap = gaps[i][j].IyIx[0];
                    m = Iy_MATRIX;
                }
                iA = i - gap;
                while (iA < i) M[--i][j].path = VERTICAL;
                M[i][j].path = VERTICAL;
                break;
            case Iy_MATRIX:
                gap = gaps[i][j].MIy[0];
                if (gap) m = M_MATRIX;
                else {
                    gap = gaps[i][j].IxIy[0];
                    m = Ix_MATRIX;
                }
                iB = j - gap;
                while (iB < j) M[i][--j].path = HORIZONTAL;
                M[i][j].path = HORIZONTAL;
                break;
        }
    }
}

static PyObject*
PathGenerator_next_waterman_smith_beyer_local(PathGenerator* self)
{
    int i, j, m;
    int trace = 0;
    int* gapM;
    int* gapXY;

    int iA = self->iA;
    int iB = self->iB;
    const int nA = self->nA;
    const int nB = self->nB;
    Trace** M = self->M;
    TraceGapsWatermanSmithBeyer** gaps = self->gaps.waterman_smith_beyer;

    int gap;
    int path = M[0][0].path;

    if (path == DONE) return NULL;
    m = 0;
    path = M[iA][iB].path;
    if (path) {
        /* We already have a path. Prune the path to see if there are
         * any alternative paths. */
        m = M_MATRIX;
        i = iA;
        j = iB;
        while (1) {
            path = M[i][j].path;
            switch (path) {
                case HORIZONTAL:
                    iA = i;
                    iB = j;
                    while (M[i][iB].path == HORIZONTAL) iB++;
                    break;
                case VERTICAL:
                    iA = i;
                    iB = j;
                    while (M[iA][j].path == VERTICAL) iA++;
                    break;
                case DIAGONAL:
                    iA = i + 1;
                    iB = j + 1;
                    break;
                default:
                    iA = -1;
                    break;
            }
            if (iA < 0) {
                m = 0;
                iA = i;
                iB = j;
                break;
            }
            if (i == iA) { /* HORIZONTAL */
                gapM = gaps[iA][iB].MIy;
                gapXY = gaps[iA][iB].IxIy;
                if (m == M_MATRIX) {
                    gap = iB - j;
                    while (*gapM != gap) gapM++;
                    gapM++;
                    gap = *gapM;
                    if (gap) {
                        j = iB - gap;
                        while (j < iB) M[i][--iB].path = HORIZONTAL;
                        break;
                    }
                } else if (m == Ix_MATRIX) {
                    gap = iB - j;
                    while (*gapXY != gap) gapXY++;
                    gapXY++;
                }
                gap = *gapXY;
                if (gap) {
                    m = Ix_MATRIX;
                    j = iB - gap;
                    M[i][j].path = HORIZONTAL;
                    while (iB > j) M[i][--iB].path = HORIZONTAL;
                    break;
                }
                /* no alternative found; continue pruning */
                m = Iy_MATRIX;
                j = iB;
            }
            else if (j == iB) { /* VERTICAL */
                gapM = gaps[iA][iB].MIx;
                gapXY = gaps[iA][iB].IyIx;
                if (m == M_MATRIX) {
                    gap = iA - i;
                    while (*gapM != gap) gapM++;
                    gapM++;
                    gap = *gapM;
                    if (gap) {
                        i = iA - gap;
                        while (i < iA) M[--iA][j].path = VERTICAL;
                        break;
                    }
                } else if (m == Iy_MATRIX) {
                    gap = iA - i;
                    while (*gapXY != gap) gapXY++;
                    gapXY++;
                }
                gap = *gapXY;
                if (gap) {
                    m = Iy_MATRIX;
                    i = iA - gap;
                    M[i][j].path = VERTICAL;
                    while (iA > i) M[--iA][j].path = VERTICAL;
                    break;
                }
                /* no alternative found; continue pruning */
                m = Ix_MATRIX;
                i = iA;
            }
            else { /* DIAGONAL */
                i = iA - 1;
                j = iB - 1;
                trace = M[iA][iB].trace;
                switch (m) {
                    case M_MATRIX:
                        if (trace & Ix_MATRIX) {
                            m = Ix_MATRIX;
                            M[i][j].path = DIAGONAL;
                            break;
                        }
                    case Ix_MATRIX:
                        if (trace & Iy_MATRIX) {
                            m = Iy_MATRIX;
                            M[i][j].path = DIAGONAL;
                            break;
                        }
                    case Iy_MATRIX:
                    default:
                        /* no alternative found; continue pruning */
                        m = M_MATRIX;
                        i = iA;
                        j = iB;
                        continue;
                }
                /* alternative found; build path until starting point */
                break;
            }
        }
    }
 
    if (m == 0) {
        /* We are at [nA][nB]. Find a suitable end point for a path. */
        while (1) {
            if (iB < nB) iB++;
            else if (iA < nA) {
                iA++;
                iB = 0;
            }
            else {
                /* exhausted this generator */
                M[0][0].path = DONE;
                return NULL;
            }
            if (M[iA][iB].trace & ENDPOINT) break;
        }
        M[iA][iB].path = 0;
        m = M_MATRIX;
        i = iA;
        j = iB;
    }

    /* Follow the traceback until we reach the origin. */
    while (1) {
        switch (m) {
            case Ix_MATRIX:
                gapM = gaps[i][j].MIx;
                gapXY = gaps[i][j].IyIx;
                iB = j;
                gap = *gapM;
                if (gap) m = M_MATRIX;
                else {
                    gap = *gapXY;
                    m = Iy_MATRIX;
                }
                iA = i - gap;
                while (i > iA) M[--i][iB].path = VERTICAL;
                break;
            case Iy_MATRIX:
                gapM = gaps[i][j].MIy;
                gapXY = gaps[i][j].IxIy;
                iA = i;
                gap = *gapM;
                if (gap) m = M_MATRIX;
                else {
                    gap = *gapXY;
                    m = Ix_MATRIX;
                }
                iB = j - gap;
                while (j > iB) M[iA][--j].path = HORIZONTAL;
                break;
            case M_MATRIX:
                iA = i-1;
                iB = j-1;
                trace = M[i][j].trace;
                if (trace & M_MATRIX) m = M_MATRIX;
                else if (trace & Ix_MATRIX) m = Ix_MATRIX;
                else if (trace & Iy_MATRIX) m = Iy_MATRIX;
                else if (trace == STARTPOINT) {
                    self->iA = i;
                    self->iB = j;
                    return _create_path(M, i, j);
                }
                else {
                    PyErr_SetString(PyExc_RuntimeError,
                        "Unexpected trace in PathGenerator_next_waterman_smith_beyer_local");
                    return NULL;
                }
                M[iA][iB].path = DIAGONAL;
                break;
        }
        i = iA;
        j = iB;
    }
}

static PyObject *
PathGenerator_next(PathGenerator* self)
{
    const Mode mode = self->mode;
    const Algorithm algorithm = self->algorithm;
    switch (algorithm) {
        case NeedlemanWunschSmithWaterman:
            switch (mode) {
                case Global:
                    return PathGenerator_next_needlemanwunsch(self);
                case Local:
                    return PathGenerator_next_smithwaterman(self);
            }
        case Gotoh:
            switch (mode) {
                case Global:
                    return PathGenerator_next_gotoh_global(self);
                case Local:
                    return PathGenerator_next_gotoh_local(self);
            }
        case WatermanSmithBeyer:
            switch (mode) {
                case Global:
                    return PathGenerator_next_waterman_smith_beyer_global(self);
                case Local:
                    return PathGenerator_next_waterman_smith_beyer_local(self);
            }
        case Unknown:
        default:
            PyErr_SetString(PyExc_RuntimeError, "Unknown algorithm");
            return NULL;
    }
}

static const char PathGenerator_reset__doc__[] = "reset the iterator";

static PyObject*
PathGenerator_reset(PathGenerator* self)
{
    switch (self->mode) {
        case Local:
            self->iA = 0;
            self->iB = 0;
        case Global: {
            Trace** M = self->M;
            switch (self->algorithm) {
                case NeedlemanWunschSmithWaterman:
                case Gotoh: {
                    if (M[0][0].path != NONE) M[0][0].path = 0;
                    break;
                }
                case WatermanSmithBeyer: {
                    M[0][0].path = 0;
                    break;
                }
                case Unknown:
                default:
                    break;
            }
        }
    }
    Py_INCREF(Py_None);
    return Py_None;
}

static PyMethodDef PathGenerator_methods[] = {
    {"reset",
     (PyCFunction)PathGenerator_reset,
     METH_NOARGS,
     PathGenerator_reset__doc__
    },
    {NULL}  /* Sentinel */
};

static PySequenceMethods PathGenerator_as_sequence = {
    (lenfunc)PathGenerator_length,  /* sq_length */
    NULL,                           /* sq_concat */
    NULL,                           /* sq_repeat */
    NULL,                           /* sq_item */
    NULL,                           /* sq_ass_item */
    NULL,                           /* sq_contains */
    NULL,                           /* sq_inplace_concat */
    NULL,                           /* sq_inplace_repeat */
};

static PyTypeObject PathGenerator_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "Path generator",               /* tp_name */
    sizeof(PathGenerator),          /* tp_basicsize */
    0,                              /* tp_itemsize */
    (destructor)PathGenerator_dealloc,  /* tp_dealloc */
    0,                              /* tp_print */
    0,                              /* tp_getattr */
    0,                              /* tp_setattr */
    0,                              /* tp_reserved */
    0,                              /* tp_repr */
    0,                              /* tp_as_number */
    &PathGenerator_as_sequence,     /* tp_as_sequence */
    0,                              /* tp_as_mapping */
    0,                              /* tp_hash */
    0,                              /* tp_call */
    0,                              /* tp_str */
    0,                              /* tp_getattro */
    0,                              /* tp_setattro */
    0,                              /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,             /* tp_flags */
    0,                              /* tp_doc */
    0,                              /* tp_traverse */
    0,                              /* tp_clear */
    0,                              /* tp_richcompare */
    0,                              /* tp_weaklistoffset */
    PyObject_SelfIter,              /* tp_iter */
    (iternextfunc)PathGenerator_next,      /* tp_iternext */
    PathGenerator_methods,          /* tp_methods */
};

typedef struct {
    PyObject_HEAD
    Mode mode;
    Algorithm algorithm;
    double match;
    double mismatch;
    double epsilon;
    double target_open_gap_score;
    double target_extend_gap_score;
    double target_left_open_gap_score;
    double target_left_extend_gap_score;
    double target_right_open_gap_score;
    double target_right_extend_gap_score;
    double query_open_gap_score;
    double query_extend_gap_score;
    double query_left_open_gap_score;
    double query_left_extend_gap_score;
    double query_right_open_gap_score;
    double query_right_extend_gap_score;
    PyObject* target_gap_function;
    PyObject* query_gap_function;
    double substitution_matrix[26][26]; /* 26 letters in the alphabet */
    int* letters;
} Aligner;

static int
Aligner_init(Aligner *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"match", "mismatch", NULL};
    int i, j;
    const int n = 26;

    self->mode = Global;
    self->match = 1.0;
    self->mismatch = 0.0;
    self->epsilon = 1.e-6;
    self->target_open_gap_score = 0;
    self->target_extend_gap_score = 0;
    self->query_open_gap_score = 0;
    self->query_extend_gap_score = 0;
    self->target_left_open_gap_score = 0;
    self->target_left_extend_gap_score = 0;
    self->target_right_open_gap_score = 0;
    self->target_right_extend_gap_score = 0;
    self->query_left_open_gap_score = 0;
    self->query_left_extend_gap_score = 0;
    self->query_right_open_gap_score = 0;
    self->query_right_extend_gap_score = 0;
    self->target_gap_function = NULL;
    self->query_gap_function = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|dd", kwlist,
                                     &self->match, &self->mismatch))
        return -1;
    for (i = 0; i < n; i++) {
        self->substitution_matrix[i][i] = self->match;
        for (j = 0; j < i; j++) {
            self->substitution_matrix[i][j] = self->mismatch;
            self->substitution_matrix[j][i] = self->mismatch;
        }
    }
    i = 'X' - 'A';
    self->substitution_matrix[i][i] = 0.0;
    self->letters = NULL;
    self->algorithm = Unknown;
    return 0;
}

static void
Aligner_dealloc(Aligner* self)
{   if (self->letters) PyMem_Free(self->letters);
    Py_XDECREF(self->target_gap_function);
    Py_XDECREF(self->query_gap_function);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject*
Aligner_repr(Aligner* self)
{
  const char text[] = "Pairwise aligner, implementing the Needleman-Wunsch, Smith-Waterman, Gotoh, and Waterman-Smith-Beyer global and local alignment algorithms";
#if PY_MAJOR_VERSION >= 3
  return PyUnicode_FromString(text);
#else
  return PyString_FromString(text);
#endif
}

static PyObject*
Aligner_str(Aligner* self)
{
    int n;
    char text[1024];
    char* p = text;
    n = sprintf(text, "Pairwise sequence aligner with parameters\n");
    p += n;
    if (self->letters) {
        n = sprintf(p, "  match/mismatch_score: <substitution matrix>\n");
        p += n;
    } else {
        n = sprintf(p, "  match_score: %f\n", self->match);
        p += n;
        n = sprintf(p, "  mismatch_score: %f\n", self->mismatch);
        p += n;
    }
    if (self->target_gap_function) {
#if PY_MAJOR_VERSION >= 3
        n = sprintf(p, "  target_gap_function: %%R\n");
        p += n;
#else
        char* s;
        PyObject* representation = PyObject_Repr(self->target_gap_function);
        if (!representation) return PyErr_NoMemory();
        s = PyString_AsString(representation);
        n = sprintf(p, "  target_gap_function: %s\n", s);
        p += n;
        Py_DECREF(representation);
#endif
    }
    else {
        n = sprintf(p, "  target_open_gap_score: %f\n",
                       self->target_open_gap_score);
        p += n;
        n = sprintf(p, "  target_extend_gap_score: %f\n",
                       self->target_extend_gap_score);
        p += n;
        n = sprintf(p, "  target_left_open_gap_score: %f\n",
                       self->target_left_open_gap_score);
        p += n;
        n = sprintf(p, "  target_left_extend_gap_score: %f\n",
                       self->target_left_extend_gap_score);
        p += n;
        n = sprintf(p, "  target_right_open_gap_score: %f\n",
                       self->target_right_open_gap_score);
        p += n;
        n = sprintf(p, "  target_right_extend_gap_score: %f\n",
                       self->target_right_extend_gap_score);
        p += n;
    }
    if (self->query_gap_function) {
#if PY_MAJOR_VERSION >= 3
        n = sprintf(p, "  query_gap_function: %%R\n");
        p += n;
#else
        char* s;
        PyObject* representation = PyObject_Repr(self->query_gap_function);
        if (!representation) return PyErr_NoMemory();
        s = PyString_AsString(representation);
        n = sprintf(p, "  query_gap_function: %s\n", s);
        p += n;
        Py_DECREF(representation);
#endif
    }
    else {
        n = sprintf(p, "  query_open_gap_score: %f\n",
                       self->query_open_gap_score);
        p += n;
        n = sprintf(p, "  query_extend_gap_score: %f\n",
                       self->query_extend_gap_score);
        p += n;
        n = sprintf(p, "  query_left_open_gap_score: %f\n",
                       self->query_left_open_gap_score);
        p += n;
        n = sprintf(p, "  query_left_extend_gap_score: %f\n",
                       self->query_left_extend_gap_score);
        p += n;
        n = sprintf(p, "  query_right_open_gap_score: %f\n",
                       self->query_right_open_gap_score);
        p += n;
        n = sprintf(p, "  query_right_extend_gap_score: %f\n",
                       self->query_right_extend_gap_score);
        p += n;
    }
    switch (self->mode) {
        case Global: n = sprintf(p, "  mode: global\n"); break;
        case Local: n = sprintf(p, "  mode: local\n"); break;
    }
    p += n;
#if PY_MAJOR_VERSION >= 3
    if (self->target_gap_function || self->query_gap_function)
        return PyUnicode_FromFormat(text, self->target_gap_function, self->query_gap_function);
    else if (self->target_gap_function)
        return PyUnicode_FromFormat(text, self->target_gap_function);
    else if (self->query_gap_function)
        return PyUnicode_FromFormat(text, self->query_gap_function);
    else
        return PyUnicode_FromString(text);
#else
    return PyString_FromString(text);
#endif
}

static char Aligner_mode__doc__[] = "alignment mode ('global' or 'local')";

static PyObject*
Aligner_get_mode(Aligner* self, void* closure)
{   const char* message = NULL;
    switch (self->mode) {
        case Global: message = "global"; break;
        case Local: message = "local"; break;
    }
#if PY_MAJOR_VERSION >= 3
    return PyUnicode_FromString(message);
#else
    return PyString_FromString(message);
#endif
}

static int
Aligner_set_mode(Aligner* self, PyObject* value, void* closure)
{
#if PY_MAJOR_VERSION >= 3
    if (PyUnicode_Check(value)) {
#else
    char* mode;
    if (PyString_Check(value)) {
#endif
#if PY_MAJOR_VERSION >= 3
        if (PyUnicode_CompareWithASCIIString(value, "global") == 0) {
            self->mode = Global;
            return 0;
        }
        if (PyUnicode_CompareWithASCIIString(value, "local") == 0) {
            self->mode = Local;
            return 0;
        }
#else
        mode = PyString_AsString(value);
        if (strcmp(mode, "global") == 0) {
            self->mode = Global;
            return 0;
        }
        if (strcmp(mode, "local") == 0) {
            self->mode = Local;
            return 0;
        }
#endif
    }
    PyErr_SetString(PyExc_ValueError,
                    "invalid mode (expected 'global' or 'local'");
    return -1;
}

static char Aligner_match_score__doc__[] = "match score";

static PyObject*
Aligner_get_match_score(Aligner* self, void* closure)
{   if (self->letters) {
        PyErr_SetString(PyExc_ValueError, "using a substitution matrix");
        return NULL;
    }
    return PyFloat_FromDouble(self->match);
}

static int
Aligner_set_match_score(Aligner* self, PyObject* value, void* closure)
{   int i;
    const int n = 26;
    const double match = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) {
        PyErr_SetString(PyExc_ValueError, "invalid match score");
        return -1;
    }
    self->match = match;
    for (i = 0; i < n; i++) self->substitution_matrix[i][i] = match;
    i = 'X' - 'A';
    self->substitution_matrix[i][i] = 0.0;
    if (self->letters) {
        PyMem_Free(self->letters);
        self->letters = NULL;
    }
    return 0;
}

static char Aligner_mismatch_score__doc__[] = "mismatch score";

static PyObject*
Aligner_get_mismatch_score(Aligner* self, void* closure)
{   if (self->letters) {
        PyErr_SetString(PyExc_ValueError, "using a substitution matrix");
        return NULL;
    }
    return PyFloat_FromDouble(self->mismatch);
}

static int
Aligner_set_mismatch_score(Aligner* self, PyObject* value, void* closure)
{   int i, j;
    const int n = 26;
    const double mismatch = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) {
        PyErr_SetString(PyExc_ValueError, "invalid match score");
        return -1;
    }
    self->mismatch = mismatch;
    for (i = 0; i < n; i++) {
        for (j = 0; j < i; j++) {
            self->substitution_matrix[i][j] = mismatch;
            self->substitution_matrix[j][i] = mismatch;
        }
    }
    i = 'X' - 'A';
    for (j = 0; j < n; j++) {
        self->substitution_matrix[i][j] = 0;
        self->substitution_matrix[j][i] = 0;
    }
    if (self->letters) {
        PyMem_Free(self->letters);
        self->letters = NULL;
    }
    return 0;
}

static char Aligner_substitution_matrix__doc__[] = "substitution_matrix";

static PyObject*
Aligner_get_substitution_matrix(Aligner* self, void* closure)
{   if (!self->letters) {
        PyErr_SetString(PyExc_ValueError, "using affine gap scores");
        return NULL;
    }
    else {
        int i, j;
        const int n = 26;
        const int* letters = self->letters;
        PyObject* key = NULL;
        PyObject* value = NULL;
        PyObject* matrix = PyDict_New();
        if (!matrix) goto exit;
        for (i = 0; i < n; i++) {
            if (!letters[i]) continue;
            for (j = 0; j < n; j++) {
                if (!letters[j]) continue;
#if PY_MAJOR_VERSION >= 3
                key = Py_BuildValue("(CC)", 'A' + i, 'A' + j);
#else
                key = Py_BuildValue("(cc)", 'A' + i, 'A' + j);
#endif
                if (!key) goto exit;
                value = PyFloat_FromDouble(self->substitution_matrix[i][j]);
                if (!value) goto exit;
                if (PyDict_SetItem(matrix, key, value) == -1) goto exit;
                Py_DECREF(key);
                Py_DECREF(value);
                value = NULL;
            }
        }
        return matrix;
exit:
        Py_XDECREF(matrix);
        Py_XDECREF(key);
        Py_XDECREF(value);
        return NULL;
    }
}

static int
Aligner_set_substitution_matrix(Aligner* self, PyObject* values, void* closure)
{   int i, j;
    const int n = 26;
    PyObject* key;
    PyObject* value;
    Py_ssize_t pos = 0;
    double score;
    double substitution_matrix[26][26];
    int substitution_matrix_flags[26][26];
    int letters[26];
    PyObject* item;
    if (!PyDict_Check(values)) {
        PyErr_SetString(PyExc_ValueError, "expected a dictionary");
        return -1;
    }
    for (i = 0; i < n; i++) {
        letters[i] = 0;
        for (j = 0; j < n; j++) substitution_matrix_flags[i][j] = 0;
    }
    while (PyDict_Next(values, &pos, &key, &value)) {
        score = PyFloat_AsDouble(value);
        if (PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError, "invalid score found");
            return -1;
        }
        if (!PyTuple_Check(key) || PyTuple_GET_SIZE(key) != 2) {
            PyErr_SetString(PyExc_ValueError,
                            "each key should be a tuple of two letters");
            return -1;
        }
        item = PyTuple_GET_ITEM(key, 0);
        i = _convert_single_letter(item);
        if (i < 0) return -1;
        item = PyTuple_GET_ITEM(key, 1);
        j = _convert_single_letter(item);
        if (j < 0) return -1;
        if (substitution_matrix_flags[i][j]) {
            i = 'A' + i;
            j = 'A' + j;
            PyErr_Format(PyExc_ValueError,
                         "score for (%c,%c) specified more than once (substitution matrix is case-insensitive)",
                         i, j);
            return -1;
        }
        substitution_matrix_flags[i][j] = 1;
        substitution_matrix[i][j] = score;
        letters[i] = 1;
        letters[j] = 1;
    }
    if (!self->letters) self->letters = PyMem_Malloc(n*sizeof(int));
    if (!self->letters) {
        PyErr_SetNone(PyExc_MemoryError);
        return -1;
    }
    /* No errors - store the new substitution matrix */
    for (i = 0; i < n; i++) {
        self->letters[i] = letters[i];
        for (j = 0; j < n; j++) {
            if (!letters[i] || !letters[j]) continue;
            if (substitution_matrix_flags[i][j])
                score = substitution_matrix[i][j];
            else if (substitution_matrix_flags[j][i])
                score = substitution_matrix[j][i];
            else
                score = 0;
            self->substitution_matrix[i][j] = score;
        }
    }
    return 0;
}

static char Aligner_gap_score__doc__[] = "gap score";

static PyObject*
Aligner_get_gap_score(Aligner* self, void* closure)
{   
    if (self->target_gap_function || self->query_gap_function) {
        if (self->target_gap_function != self->query_gap_function) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        Py_INCREF(self->target_gap_function);
        return self->target_gap_function;
    }
    else {
        const double score = self->target_open_gap_score;
        if (score != self->target_extend_gap_score
         || score != self->target_left_open_gap_score
         || score != self->target_left_extend_gap_score
         || score != self->target_right_open_gap_score
         || score != self->target_right_extend_gap_score
         || score != self->query_open_gap_score
         || score != self->query_extend_gap_score
         || score != self->query_left_open_gap_score
         || score != self->query_left_extend_gap_score
         || score != self->query_right_open_gap_score
         || score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_gap_score(Aligner* self, PyObject* value, void* closure)
{   if (PyCallable_Check(value)) {
        Py_XDECREF(self->target_gap_function);
        Py_XDECREF(self->query_gap_function);
        Py_INCREF(value);
        Py_INCREF(value);
        self->target_gap_function = value;
        self->query_gap_function = value;
    }
    else {
        const double score = PyFloat_AsDouble(value);
        if (PyErr_Occurred()) return -1;
        if (self->target_gap_function) {
            Py_DECREF(self->target_gap_function);
            self->target_gap_function = NULL;
        }
        if (self->query_gap_function) {
            Py_DECREF(self->query_gap_function);
            self->query_gap_function = NULL;
        }
        self->target_open_gap_score = score;
        self->target_extend_gap_score = score;
        self->target_left_open_gap_score = score;
        self->target_left_extend_gap_score = score;
        self->target_right_open_gap_score = score;
        self->target_right_extend_gap_score = score;
        self->query_open_gap_score = score;
        self->query_extend_gap_score = score;
        self->query_left_open_gap_score = score;
        self->query_left_extend_gap_score = score;
        self->query_right_open_gap_score = score;
        self->query_right_extend_gap_score = score;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_open_gap_score__doc__[] = "internal and end open gap score";

static PyObject*
Aligner_get_open_gap_score(Aligner* self, void* closure)
{   
    if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_open_gap_score;
        if (score != self->target_left_open_gap_score
         || score != self->target_right_open_gap_score
         || score != self->query_open_gap_score
         || score != self->query_left_open_gap_score
         || score != self->query_right_open_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_open_gap_score = score;
    self->target_left_open_gap_score = score;
    self->target_right_open_gap_score = score;
    self->query_open_gap_score = score;
    self->query_left_open_gap_score = score;
    self->query_right_open_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_extend_gap_score__doc__[] = "extend gap score";

static PyObject*
Aligner_get_extend_gap_score(Aligner* self, void* closure)
{   
    if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_extend_gap_score;
        if (score != self->target_left_extend_gap_score
         || score != self->target_right_extend_gap_score
         || score != self->query_extend_gap_score
         || score != self->query_left_extend_gap_score
         || score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_extend_gap_score = score;
    self->target_left_extend_gap_score = score;
    self->target_right_extend_gap_score = score;
    self->query_extend_gap_score = score;
    self->query_left_extend_gap_score = score;
    self->query_right_extend_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_internal_gap_score__doc__[] = "internal gap score";

static PyObject*
Aligner_get_internal_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_open_gap_score;
        if (score != self->target_extend_gap_score
         || score != self->query_open_gap_score
         || score != self->query_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_internal_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_open_gap_score = score;
    self->target_extend_gap_score = score;
    self->query_open_gap_score = score;
    self->query_extend_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_internal_open_gap_score__doc__[] = "internal open gap score";

static PyObject*
Aligner_get_internal_open_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_open_gap_score;
        if (score != self->query_open_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_internal_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_open_gap_score = score;
    self->query_open_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_internal_extend_gap_score__doc__[] = "internal extend gap score";

static PyObject*
Aligner_get_internal_extend_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_extend_gap_score;
        if (score != self->query_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_internal_extend_gap_score(Aligner* self, PyObject* value,
                                      void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_extend_gap_score = score;
    self->query_extend_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_end_gap_score__doc__[] = "end gap score";

static PyObject*
Aligner_get_end_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_left_open_gap_score;
        if (score != self->target_left_extend_gap_score
         || score != self->target_right_open_gap_score
         || score != self->target_right_extend_gap_score
         || score != self->query_left_open_gap_score
         || score != self->query_left_extend_gap_score
         || score != self->query_right_open_gap_score
         || score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_end_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_left_open_gap_score = score;
    self->target_left_extend_gap_score = score;
    self->target_right_open_gap_score = score;
    self->target_right_extend_gap_score = score;
    self->query_left_open_gap_score = score;
    self->query_left_extend_gap_score = score;
    self->query_right_open_gap_score = score;
    self->query_right_extend_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_end_open_gap_score__doc__[] = "end open gap score";

static PyObject*
Aligner_get_end_open_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_left_open_gap_score;
        if (score != self->target_right_open_gap_score
         || score != self->query_left_open_gap_score
         || score != self->query_right_open_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_end_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_left_open_gap_score = score;
    self->target_right_open_gap_score = score;
    self->query_left_open_gap_score = score;
    self->query_right_open_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_end_extend_gap_score__doc__[] = "end extend gap score";

static PyObject*
Aligner_get_end_extend_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_left_extend_gap_score;
        if (score != self->target_right_extend_gap_score
         || score != self->query_left_extend_gap_score
         || score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_end_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_left_extend_gap_score = score;
    self->target_right_extend_gap_score = score;
    self->query_left_extend_gap_score = score;
    self->query_right_extend_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_left_gap_score__doc__[] = "left gap score";

static PyObject*
Aligner_get_left_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_left_open_gap_score;
        if (score != self->target_left_extend_gap_score
         || score != self->query_left_open_gap_score
         || score != self->query_left_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_left_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_left_open_gap_score = score;
    self->target_left_extend_gap_score = score;
    self->query_left_open_gap_score = score;
    self->query_left_extend_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_right_gap_score__doc__[] = "right gap score";

static PyObject*
Aligner_get_right_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_right_open_gap_score;
        if (score != self->target_right_extend_gap_score
         || score != self->query_right_open_gap_score
         || score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_right_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_right_open_gap_score = score;
    self->target_right_extend_gap_score = score;
    self->query_right_open_gap_score = score;
    self->query_right_extend_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_left_open_gap_score__doc__[] = "left open gap score";

static PyObject*
Aligner_get_left_open_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_left_open_gap_score;
        if (score != self->query_left_open_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_left_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_left_open_gap_score = score;
    self->query_left_open_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_left_extend_gap_score__doc__[] = "left extend gap score";

static PyObject*
Aligner_get_left_extend_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_left_extend_gap_score;
        if (score != self->query_left_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_left_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_left_extend_gap_score = score;
    self->query_left_extend_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_right_open_gap_score__doc__[] = "right open gap score";

static PyObject*
Aligner_get_right_open_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_right_open_gap_score;
        if (score != self->query_right_open_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_right_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_right_open_gap_score = score;
    self->query_right_open_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_right_extend_gap_score__doc__[] = "right extend gap score";

static PyObject*
Aligner_get_right_extend_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function || self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_right_extend_gap_score;
        if (score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_right_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->target_right_extend_gap_score = score;
    self->query_right_extend_gap_score = score;
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_open_gap_score__doc__[] = "target open gap score";

static PyObject*
Aligner_get_target_open_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_open_gap_score;
        if (score != self->target_left_open_gap_score
         || score != self->target_right_open_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_target_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_open_gap_score = score;
    self->target_left_open_gap_score = score;
    self->target_right_open_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_extend_gap_score__doc__[] = "target extend gap score";

static PyObject*
Aligner_get_target_extend_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_extend_gap_score;
        if (score != self->target_left_extend_gap_score
         || score != self->target_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_target_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_extend_gap_score = score;
    self->target_left_extend_gap_score = score;
    self->target_right_extend_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_gap_score__doc__[] = "target gap score";

static PyObject*
Aligner_get_target_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        Py_INCREF(self->target_gap_function);
        return self->target_gap_function;
    }
    else {
        const double score = self->target_open_gap_score;
        if (score != self->target_extend_gap_score
         || score != self->target_left_open_gap_score
         || score != self->target_left_extend_gap_score
         || score != self->target_right_open_gap_score
         || score != self->target_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_target_gap_score(Aligner* self, PyObject* value, void* closure)
{
    if (PyCallable_Check(value)) {
        Py_XDECREF(self->target_gap_function);
        Py_INCREF(value);
        self->target_gap_function = value;
    }
    else {
        const double score = PyFloat_AsDouble(value);
        if (PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError,
                            "gap score should be numerical or callable");
            return -1;
        }
        self->target_open_gap_score = score;
        self->target_extend_gap_score = score;
        self->target_left_open_gap_score = score;
        self->target_left_extend_gap_score = score;
        self->target_right_open_gap_score = score;
        self->target_right_extend_gap_score = score;
        if (self->target_gap_function) {
            Py_DECREF(self->target_gap_function);
            self->target_gap_function = NULL;
        }
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_open_gap_score__doc__[] = "query gap open score";

static PyObject*
Aligner_get_query_open_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->query_open_gap_score;
        if (score != self->query_left_open_gap_score
         || score != self->query_right_open_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_query_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_open_gap_score = score;
    self->query_left_open_gap_score = score;
    self->query_right_open_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_extend_gap_score__doc__[] = "query gap extend score";

static PyObject*
Aligner_get_query_extend_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->query_extend_gap_score;
        if (score != self->query_left_extend_gap_score
         || score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_query_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_extend_gap_score = score;
    self->query_left_extend_gap_score = score;
    self->query_right_extend_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_gap_score__doc__[] = "query gap score";

static PyObject*
Aligner_get_query_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        Py_INCREF(self->query_gap_function);
        return self->query_gap_function;
    }
    else {
        const double score = self->query_open_gap_score;
        if (score != self->query_left_open_gap_score
         || score != self->query_right_open_gap_score
         || score != self->query_extend_gap_score
         || score != self->query_left_extend_gap_score
         || score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_query_gap_score(Aligner* self, PyObject* value, void* closure)
{   if (PyCallable_Check(value)) {
        Py_XDECREF(self->query_gap_function);
        Py_INCREF(value);
        self->query_gap_function = value;
    }
    else {
        const double score = PyFloat_AsDouble(value);
        if (PyErr_Occurred()) {
            PyErr_SetString(PyExc_ValueError,
                            "gap score should be numerical or callable");
            return -1;
        }
        self->query_open_gap_score = score;
        self->query_extend_gap_score = score;
        self->query_left_open_gap_score = score;
        self->query_left_extend_gap_score = score;
        self->query_right_open_gap_score = score;
        self->query_right_extend_gap_score = score;
        if (self->query_gap_function) {
            Py_DECREF(self->query_gap_function);
            self->query_gap_function = NULL;
        }
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_internal_open_gap_score__doc__[] = "target internal open gap score";

static PyObject*
Aligner_get_target_internal_open_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->target_open_gap_score);
}

static int
Aligner_set_target_internal_open_gap_score(Aligner* self,
                                           PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_open_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_internal_extend_gap_score__doc__[] = "target internal extend gap score";

static PyObject*
Aligner_get_target_internal_extend_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->target_extend_gap_score);
}

static int
Aligner_set_target_internal_extend_gap_score(Aligner* self,
                                             PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_extend_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_internal_gap_score__doc__[] = "target internal gap score";

static PyObject*
Aligner_get_target_internal_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_open_gap_score;
        if (score != self->target_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_target_internal_gap_score(Aligner* self, PyObject* value,
                                      void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_open_gap_score = score;
    self->target_extend_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_end_gap_score__doc__[] = "target end gap score";

static PyObject*
Aligner_get_target_end_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_left_open_gap_score;
        if (score != self->target_left_extend_gap_score
         || score != self->target_right_open_gap_score
         || score != self->target_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_target_end_gap_score(Aligner* self, PyObject* value, void* closure) {
    const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_left_open_gap_score = score;
    self->target_left_extend_gap_score = score;
    self->target_right_open_gap_score = score;
    self->target_right_extend_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_end_open_gap_score__doc__[] = "target end open gap score";

static PyObject*
Aligner_get_target_end_open_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_left_open_gap_score;
        if (score != self->target_right_open_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_target_end_open_gap_score(Aligner* self, PyObject* value,
                                      void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_left_open_gap_score = score;
    self->target_right_open_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_end_extend_gap_score__doc__[] = "target end extend gap score";

static PyObject*
Aligner_get_target_end_extend_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_left_extend_gap_score;
        if (score != self->target_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_target_end_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_left_extend_gap_score = score;
    self->target_right_extend_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_left_open_gap_score__doc__[] = "target left open score";

static PyObject*
Aligner_get_target_left_open_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->target_left_open_gap_score);
}

static int
Aligner_set_target_left_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_left_open_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_left_extend_gap_score__doc__[] = "target left extend score";

static PyObject*
Aligner_get_target_left_extend_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->target_left_extend_gap_score);
}

static int
Aligner_set_target_left_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_left_extend_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_left_gap_score__doc__[] = "target left score";

static PyObject*
Aligner_get_target_left_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_left_open_gap_score;
        if (score != self->target_left_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_target_left_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_left_open_gap_score = score;
    self->target_left_extend_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_right_gap_score_open__doc__[] = "target right open score";

static PyObject*
Aligner_get_target_right_open_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->target_right_open_gap_score);
}

static int
Aligner_set_target_right_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_right_open_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_right_extend_gap_score__doc__[] = "target right extend score";

static PyObject*
Aligner_get_target_right_extend_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->target_right_extend_gap_score);
}

static int
Aligner_set_target_right_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_right_extend_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_target_right_gap_score__doc__[] = "target right score";

static PyObject*
Aligner_get_target_right_gap_score(Aligner* self, void* closure)
{   if (self->target_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->target_right_open_gap_score;
        if (score != self->target_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_target_right_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->target_right_open_gap_score = score;
    self->target_right_extend_gap_score = score;
    if (self->target_gap_function) {
        Py_DECREF(self->target_gap_function);
        self->target_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_end_gap_score__doc__[] = "query end score";

static PyObject*
Aligner_get_query_end_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->query_left_open_gap_score;
        if (score != self->query_left_extend_gap_score
         || score != self->query_right_open_gap_score
         || score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_query_end_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_left_open_gap_score = score;
    self->query_left_extend_gap_score = score;
    self->query_right_open_gap_score = score;
    self->query_right_extend_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_end_open_gap_score__doc__[] = "query end open score";

static PyObject*
Aligner_get_query_end_open_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->query_left_open_gap_score;
        if (score != self->query_right_open_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_query_end_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_left_open_gap_score = score;
    self->query_right_open_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_end_extend_gap_score__doc__[] = "query end extend score";

static PyObject*
Aligner_get_query_end_extend_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->query_left_extend_gap_score;
        if (score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_query_end_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_left_extend_gap_score = score;
    self->query_right_extend_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_internal_open_gap_score__doc__[] = "query internal open gap score";

static PyObject*
Aligner_get_query_internal_open_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->query_open_gap_score);
}

static int
Aligner_set_query_internal_open_gap_score(Aligner* self, PyObject* value,
                                          void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_open_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_internal_extend_gap_score__doc__[] = "query internal extend gap score";

static PyObject*
Aligner_get_query_internal_extend_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->query_extend_gap_score);
}

static int
Aligner_set_query_internal_extend_gap_score(Aligner* self, PyObject* value,
                                            void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_extend_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_internal_gap_score__doc__[] = "query internal gap score";

static PyObject*
Aligner_get_query_internal_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->query_open_gap_score;
        if (score != self->query_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_query_internal_gap_score(Aligner* self, PyObject* value,
                                     void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_open_gap_score = score;
    self->query_extend_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_left_open_gap_score__doc__[] = "query left open score";

static PyObject*
Aligner_get_query_left_open_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->query_left_open_gap_score);
}

static int
Aligner_set_query_left_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_left_open_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_left_extend_gap_score__doc__[] = "query left extend score";

static PyObject*
Aligner_get_query_left_extend_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->query_left_extend_gap_score);
}

static int
Aligner_set_query_left_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_left_extend_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_left_gap_score__doc__[] = "query left score";

static PyObject*
Aligner_get_query_left_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->query_left_open_gap_score;
        if (score != self->query_left_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_query_left_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_left_open_gap_score = score;
    self->query_left_extend_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_right_open_gap_score__doc__[] = "query right open score";

static PyObject*
Aligner_get_query_right_open_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->query_right_open_gap_score);
}

static int
Aligner_set_query_right_open_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_right_open_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_right_extend_gap_score__doc__[] = "query right extend score";

static PyObject*
Aligner_get_query_right_extend_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    return PyFloat_FromDouble(self->query_right_extend_gap_score);
}

static int
Aligner_set_query_right_extend_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_right_extend_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_query_right_gap_score__doc__[] = "query right score";

static PyObject*
Aligner_get_query_right_gap_score(Aligner* self, void* closure)
{   if (self->query_gap_function) {
        PyErr_SetString(PyExc_ValueError, "using a gap score function");
        return NULL;
    }
    else {
        const double score = self->query_right_open_gap_score;
        if (score != self->query_right_extend_gap_score) {
            PyErr_SetString(PyExc_ValueError, "gap scores are different");
            return NULL;
        }
        return PyFloat_FromDouble(score);
    }
}

static int
Aligner_set_query_right_gap_score(Aligner* self, PyObject* value, void* closure)
{   const double score = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->query_right_open_gap_score = score;
    self->query_right_extend_gap_score = score;
    if (self->query_gap_function) {
        Py_DECREF(self->query_gap_function);
        self->query_gap_function = NULL;
    }
    self->algorithm = Unknown;
    return 0;
}

static char Aligner_epsilon__doc__[] = "roundoff epsilon";

static PyObject*
Aligner_get_epsilon(Aligner* self, void* closure)
{   return PyFloat_FromDouble(self->epsilon);
}

static int
Aligner_set_epsilon(Aligner* self, PyObject* value, void* closure)
{   const double epsilon = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) return -1;
    self->epsilon = epsilon;
    self->algorithm = Unknown;
    return 0;
}

static Algorithm _get_algorithm(Aligner* self)
{
    Algorithm algorithm = self->algorithm;
    if (algorithm == Unknown) {
        const double target_gap_open = self->target_open_gap_score;
        const double query_gap_open = self->query_open_gap_score;
        const double target_gap_extend = self->target_extend_gap_score;
        const double query_gap_extend = self->query_extend_gap_score;
        const double target_left_open = self->target_left_open_gap_score;
        const double target_left_extend = self->target_left_extend_gap_score;
        const double query_left_open = self->query_left_open_gap_score;
        const double target_right_open = self->target_right_open_gap_score;
        const double query_right_open = self->query_right_open_gap_score;
        const double target_right_extend = self->target_right_extend_gap_score;
        const double query_left_extend = self->query_left_extend_gap_score;
        const double query_right_extend = self->query_right_extend_gap_score;
        if (self->target_gap_function || self->query_gap_function)
            algorithm = WatermanSmithBeyer;
        else if (target_gap_open == target_gap_extend
              && query_gap_open == query_gap_extend
              && target_left_open == target_left_extend
              && target_right_open == target_right_extend
              && query_left_open == query_left_extend
              && query_right_open == query_right_extend)
            algorithm = NeedlemanWunschSmithWaterman;
        else
            algorithm = Gotoh;
        self->algorithm = algorithm;
    }
    return algorithm;
}


static char Aligner_algorithm__doc__[] = "alignment algorithm";

static PyObject*
Aligner_get_algorithm(Aligner* self, void* closure)
{
    const char* s = NULL;
    const Mode mode = self->mode;
    const Algorithm algorithm = _get_algorithm(self);
    switch (algorithm) {
        case NeedlemanWunschSmithWaterman:
            switch (mode) {
                case Global:
                    s = "Needleman-Wunsch";
                    break;
                case Local:
                    s = "Smith-Waterman";
                    break;
            }
            break;
        case Gotoh:
            switch (mode) {
                case Global:
                    s = "Gotoh global alignment algorithm";
                    break;
                case Local:
                    s = "Gotoh local alignment algorithm";
                    break;
            }
            break;
        case WatermanSmithBeyer:
            switch (mode) {
                case Global:
                    s = "Waterman-Smith-Beyer global alignment algorithm";
                    break;
                case Local:
                    s = "Waterman-Smith-Beyer local alignment algorithm";
                    break;
            }
            break;
        case Unknown:
        default:
            break;
    }
#if PY_MAJOR_VERSION >= 3
    return PyUnicode_FromString(s);
#else
    return PyString_FromString(s);
#endif
}

static PyGetSetDef Aligner_getset[] = {
    {"mode",
        (getter)Aligner_get_mode,
        (setter)Aligner_set_mode,
        Aligner_mode__doc__, NULL},
    {"match_score",
        (getter)Aligner_get_match_score,
        (setter)Aligner_set_match_score,
        Aligner_match_score__doc__, NULL},
    {"mismatch_score",
        (getter)Aligner_get_mismatch_score,
        (setter)Aligner_set_mismatch_score,
        Aligner_mismatch_score__doc__, NULL},
    {"match", /* synonym for match_score */
        (getter)Aligner_get_match_score,
        (setter)Aligner_set_match_score,
        Aligner_match_score__doc__, NULL},
    {"mismatch", /* synonym for mismatch_score */
        (getter)Aligner_get_mismatch_score,
        (setter)Aligner_set_mismatch_score,
        Aligner_mismatch_score__doc__, NULL},
    {"substitution_matrix",
        (getter)Aligner_get_substitution_matrix,
        (setter)Aligner_set_substitution_matrix,
        Aligner_substitution_matrix__doc__, NULL},
    {"gap_score",
        (getter)Aligner_get_gap_score,
        (setter)Aligner_set_gap_score,
        Aligner_gap_score__doc__, NULL},
    {"open_gap_score",
        (getter)Aligner_get_open_gap_score,
        (setter)Aligner_set_open_gap_score,
        Aligner_open_gap_score__doc__, NULL},
    {"extend_gap_score",
        (getter)Aligner_get_extend_gap_score,
        (setter)Aligner_set_extend_gap_score,
        Aligner_extend_gap_score__doc__, NULL},
    {"internal_gap_score",
        (getter)Aligner_get_internal_gap_score,
        (setter)Aligner_set_internal_gap_score,
        Aligner_internal_gap_score__doc__, NULL},
    {"internal_open_gap_score",
        (getter)Aligner_get_internal_open_gap_score,
        (setter)Aligner_set_internal_open_gap_score,
        Aligner_internal_open_gap_score__doc__, NULL},
    {"internal_extend_gap_score",
        (getter)Aligner_get_internal_extend_gap_score,
        (setter)Aligner_set_internal_extend_gap_score,
        Aligner_internal_extend_gap_score__doc__, NULL},
    {"end_gap_score",
        (getter)Aligner_get_end_gap_score,
        (setter)Aligner_set_end_gap_score,
        Aligner_end_gap_score__doc__, NULL},
    {"end_open_gap_score",
        (getter)Aligner_get_end_open_gap_score,
        (setter)Aligner_set_end_open_gap_score,
        Aligner_end_open_gap_score__doc__, NULL},
    {"end_extend_gap_score",
        (getter)Aligner_get_end_extend_gap_score,
        (setter)Aligner_set_end_extend_gap_score,
        Aligner_end_extend_gap_score__doc__, NULL},
    {"left_gap_score",
        (getter)Aligner_get_left_gap_score,
        (setter)Aligner_set_left_gap_score,
        Aligner_left_gap_score__doc__, NULL},
    {"left_open_gap_score",
        (getter)Aligner_get_left_open_gap_score,
        (setter)Aligner_set_left_open_gap_score,
        Aligner_left_open_gap_score__doc__, NULL},
    {"left_extend_gap_score",
        (getter)Aligner_get_left_extend_gap_score,
        (setter)Aligner_set_left_extend_gap_score,
        Aligner_left_extend_gap_score__doc__, NULL},
    {"right_gap_score",
        (getter)Aligner_get_right_gap_score,
        (setter)Aligner_set_right_gap_score,
        Aligner_right_gap_score__doc__, NULL},
    {"right_open_gap_score",
        (getter)Aligner_get_right_open_gap_score,
        (setter)Aligner_set_right_open_gap_score,
        Aligner_right_open_gap_score__doc__, NULL},
    {"right_extend_gap_score",
        (getter)Aligner_get_right_extend_gap_score,
        (setter)Aligner_set_right_extend_gap_score,
        Aligner_right_extend_gap_score__doc__, NULL},
    {"target_open_gap_score",
        (getter)Aligner_get_target_open_gap_score,
        (setter)Aligner_set_target_open_gap_score,
        Aligner_target_open_gap_score__doc__, NULL},
    {"target_extend_gap_score",
        (getter)Aligner_get_target_extend_gap_score,
        (setter)Aligner_set_target_extend_gap_score,
        Aligner_target_extend_gap_score__doc__, NULL},
    {"target_gap_score",
        (getter)Aligner_get_target_gap_score,
        (setter)Aligner_set_target_gap_score,
        Aligner_target_gap_score__doc__, NULL},
    {"query_open_gap_score",
        (getter)Aligner_get_query_open_gap_score,
        (setter)Aligner_set_query_open_gap_score,
        Aligner_query_open_gap_score__doc__, NULL},
    {"query_extend_gap_score",
        (getter)Aligner_get_query_extend_gap_score,
        (setter)Aligner_set_query_extend_gap_score,
        Aligner_query_extend_gap_score__doc__, NULL},
    {"query_gap_score",
        (getter)Aligner_get_query_gap_score,
        (setter)Aligner_set_query_gap_score,
        Aligner_query_gap_score__doc__, NULL},
    {"target_end_gap_score",
        (getter)Aligner_get_target_end_gap_score,
        (setter)Aligner_set_target_end_gap_score,
        Aligner_target_end_gap_score__doc__, NULL},
    {"target_end_open_gap_score",
        (getter)Aligner_get_target_end_open_gap_score,
        (setter)Aligner_set_target_end_open_gap_score,
        Aligner_target_end_open_gap_score__doc__, NULL},
    {"target_end_extend_gap_score",
        (getter)Aligner_get_target_end_extend_gap_score,
        (setter)Aligner_set_target_end_extend_gap_score,
        Aligner_target_end_extend_gap_score__doc__, NULL},
    {"target_internal_open_gap_score",
        (getter)Aligner_get_target_internal_open_gap_score,
        (setter)Aligner_set_target_internal_open_gap_score,
        Aligner_target_internal_open_gap_score__doc__, NULL},
    {"target_internal_extend_gap_score",
        (getter)Aligner_get_target_internal_extend_gap_score,
        (setter)Aligner_set_target_internal_extend_gap_score,
        Aligner_target_internal_extend_gap_score__doc__, NULL},
    {"target_internal_gap_score",
        (getter)Aligner_get_target_internal_gap_score,
        (setter)Aligner_set_target_internal_gap_score,
        Aligner_target_internal_gap_score__doc__, NULL},
    {"target_left_open_gap_score",
        (getter)Aligner_get_target_left_open_gap_score,
        (setter)Aligner_set_target_left_open_gap_score,
        Aligner_target_left_open_gap_score__doc__, NULL},
    {"target_left_extend_gap_score",
        (getter)Aligner_get_target_left_extend_gap_score,
        (setter)Aligner_set_target_left_extend_gap_score,
        Aligner_target_left_extend_gap_score__doc__, NULL},
    {"target_left_gap_score",
        (getter)Aligner_get_target_left_gap_score,
        (setter)Aligner_set_target_left_gap_score,
        Aligner_target_left_gap_score__doc__, NULL},
    {"target_right_open_gap_score",
        (getter)Aligner_get_target_right_open_gap_score,
        (setter)Aligner_set_target_right_open_gap_score,
        Aligner_target_right_gap_score_open__doc__, NULL},
    {"target_right_extend_gap_score",
        (getter)Aligner_get_target_right_extend_gap_score,
        (setter)Aligner_set_target_right_extend_gap_score,
        Aligner_target_right_extend_gap_score__doc__, NULL},
    {"target_right_gap_score",
        (getter)Aligner_get_target_right_gap_score,
        (setter)Aligner_set_target_right_gap_score,
        Aligner_target_right_gap_score__doc__, NULL},
    {"query_end_gap_score",
        (getter)Aligner_get_query_end_gap_score,
        (setter)Aligner_set_query_end_gap_score,
        Aligner_query_end_gap_score__doc__, NULL},
    {"query_end_open_gap_score",
        (getter)Aligner_get_query_end_open_gap_score,
        (setter)Aligner_set_query_end_open_gap_score,
        Aligner_query_end_open_gap_score__doc__, NULL},
    {"query_end_extend_gap_score",
        (getter)Aligner_get_query_end_extend_gap_score,
        (setter)Aligner_set_query_end_extend_gap_score,
        Aligner_query_end_extend_gap_score__doc__, NULL},
    {"query_internal_open_gap_score",
        (getter)Aligner_get_query_internal_open_gap_score,
        (setter)Aligner_set_query_internal_open_gap_score,
        Aligner_query_internal_open_gap_score__doc__, NULL},
    {"query_internal_extend_gap_score",
        (getter)Aligner_get_query_internal_extend_gap_score,
        (setter)Aligner_set_query_internal_extend_gap_score,
        Aligner_query_internal_extend_gap_score__doc__, NULL},
    {"query_internal_gap_score",
        (getter)Aligner_get_query_internal_gap_score,
        (setter)Aligner_set_query_internal_gap_score,
        Aligner_query_internal_gap_score__doc__, NULL},
    {"query_left_open_gap_score",
        (getter)Aligner_get_query_left_open_gap_score,
        (setter)Aligner_set_query_left_open_gap_score,
        Aligner_query_left_open_gap_score__doc__, NULL},
    {"query_left_extend_gap_score",
        (getter)Aligner_get_query_left_extend_gap_score,
        (setter)Aligner_set_query_left_extend_gap_score,
        Aligner_query_left_extend_gap_score__doc__, NULL},
    {"query_left_gap_score",
        (getter)Aligner_get_query_left_gap_score,
        (setter)Aligner_set_query_left_gap_score,
         Aligner_query_left_gap_score__doc__, NULL},
    {"query_right_open_gap_score",
        (getter)Aligner_get_query_right_open_gap_score,
        (setter)Aligner_set_query_right_open_gap_score,
        Aligner_query_right_open_gap_score__doc__, NULL},
    {"query_right_extend_gap_score",
        (getter)Aligner_get_query_right_extend_gap_score,
        (setter)Aligner_set_query_right_extend_gap_score,
        Aligner_query_right_extend_gap_score__doc__, NULL},
    {"query_right_gap_score",
        (getter)Aligner_get_query_right_gap_score,
        (setter)Aligner_set_query_right_gap_score,
        Aligner_query_right_gap_score__doc__, NULL},
    {"epsilon",
        (getter)Aligner_get_epsilon,
        (setter)Aligner_set_epsilon,
        Aligner_epsilon__doc__, NULL},
    {"algorithm",
        (getter)Aligner_get_algorithm,
        (setter)NULL,
        Aligner_algorithm__doc__, NULL},
    {NULL}  /* Sentinel */
};

#define SELECT_SCORE_GLOBAL(score1, score2, score3) \
    score = score1; \
    temp = score2; \
    if (temp > score) score = temp; \
    temp = score3; \
    if (temp > score) score = temp;

#define SELECT_SCORE_WATERMAN_SMITH_BEYER(score1, score2) \
    temp = score1 + gapscore; \
    if (temp > score) score = temp; \
    temp = score2 + gapscore; \
    if (temp > score) score = temp;

#define SELECT_SCORE_GOTOH_LOCAL_ALIGN(score1, score2, score3, score4) \
    score = score1; \
    temp = score2; \
    if (temp > score) score = temp; \
    temp = score3; \
    if (temp > score) score = temp; \
    score += score4; \
    if (score < 0) score = 0; \
    else if (score > maximum) maximum = score;

#define SELECT_SCORE_LOCAL3(score1, score2, score3) \
    score = score1; \
    temp = score2; \
    if (temp > score) score = temp; \
    temp = score3; \
    if (temp > score) score = temp; \
    if (score < 0) score = 0; \
    else if (score > maximum) maximum = score;

#define SELECT_SCORE_LOCAL1(score1) \
    score = score1; \
    if (score < 0) score = 0; \
    else if (score > maximum) maximum = score;

#define SELECT_TRACE_NEEDLEMAN_WUNSCH(hgap, vgap) \
    score = temp + self->substitution_matrix[kA][kB]; \
    trace = DIAGONAL; \
    temp = scores[j-1] + hgap; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = HORIZONTAL; \
    } \
    else if (temp > score - epsilon) trace |= HORIZONTAL; \
    temp = scores[j] + vgap; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = VERTICAL; \
    } \
    else if (temp > score - epsilon) trace |= VERTICAL; \
    temp = scores[j]; \
    scores[j] = score; \
    M[i][j].trace = trace;

#define SELECT_TRACE_SMITH_WATERMAN_HVD(hgap, vgap) \
    trace = DIAGONAL; \
    score = temp + self->substitution_matrix[kA][kB]; \
    temp = scores[j-1] + hgap; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = HORIZONTAL; \
    } \
    else if (temp > score - epsilon) trace |= HORIZONTAL; \
    temp = scores[j] + vgap; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = VERTICAL; \
    } \
    else if (temp > score - epsilon) trace |= VERTICAL; \
    if (score < epsilon) { \
        score = 0; \
        trace = STARTPOINT; \
    } \
    else if (trace & DIAGONAL && score > maximum - epsilon) { \
        if (score > maximum + epsilon) { \
            for ( ; im < i; im++, jm = 0) \
                for ( ; jm <= nB; jm++) M[im][jm].trace &= ~ENDPOINT; \
            for ( ; jm < j; jm++) M[im][jm].trace &= ~ENDPOINT; \
            im = i; \
            jm = j; \
        } \
        trace |= ENDPOINT; \
    } \
    M[i][j].trace = trace; \
    if (score > maximum) maximum = score; \
    temp = scores[j]; \
    scores[j] = score;

#define SELECT_TRACE_SMITH_WATERMAN_D \
    score = temp + self->substitution_matrix[kA][kB]; \
    trace = DIAGONAL; \
    if (score < epsilon) { \
        score = 0; \
    } \
    else if (trace & DIAGONAL && score > maximum - epsilon) { \
        if (score > maximum + epsilon) { \
            for ( ; im < i; im++, jm = 0) \
                for ( ; jm <= nB; jm++) M[im][jm].trace &= ~ENDPOINT; \
            for ( ; jm < j; jm++) M[im][jm].trace &= ~ENDPOINT; \
            im = i; \
            jm = j; \
        } \
        trace |= ENDPOINT; \
    } \
    M[i][j].trace = trace; \
    if (score > maximum) maximum = score; \
    temp = scores[j]; \
    scores[j] = score

#define SELECT_TRACE_GOTOH_GLOBAL_GAP(matrix, score1, score2, score3) \
    trace = M_MATRIX; \
    score = score1; \
    temp = score2; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = Ix_MATRIX; \
    } \
    else if (temp > score - epsilon) trace |= Ix_MATRIX; \
    temp = score3; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = Iy_MATRIX; \
    } \
    else if (temp > score - epsilon) trace |= Iy_MATRIX; \
    gaps[i][j].matrix = trace;

#define SELECT_TRACE_GOTOH_GLOBAL_ALIGN \
    trace = M_MATRIX; \
    score = M_temp; \
    temp = Ix_temp; \
    if (temp > score + epsilon) { \
        score = Ix_temp; \
        trace = Ix_MATRIX; \
    } \
    else if (temp > score - epsilon) trace |= Ix_MATRIX; \
    temp = Iy_temp; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = Iy_MATRIX; \
    } \
    else if (temp > score - epsilon) trace |= Iy_MATRIX; \
    M[i][j].trace = trace;

#define SELECT_TRACE_GOTOH_LOCAL_ALIGN \
    trace = M_MATRIX; \
    score = M_temp; \
    if (Ix_temp > score + epsilon) { \
        score = Ix_temp; \
        trace = Ix_MATRIX; \
    } \
    else if (Ix_temp > score - epsilon) trace |= Ix_MATRIX; \
    if (Iy_temp > score + epsilon) { \
        score = Iy_temp; \
        trace = Iy_MATRIX; \
    } \
    else if (Iy_temp > score - epsilon) trace |= Iy_MATRIX; \
    score += self->substitution_matrix[kA][kB]; \
    if (score < epsilon) { \
        score = 0; \
        trace = STARTPOINT; \
    } \
    else if (score > maximum - epsilon) { \
        if (score > maximum + epsilon) { \
            maximum = score; \
            for ( ; im < i; im++, jm = 0) \
                for ( ; jm <= nB; jm++) M[im][jm].trace &= ~ENDPOINT; \
            for ( ; jm < j; jm++) M[im][jm].trace &= ~ENDPOINT; \
            im = i; \
            jm = j; \
        } \
        trace |= ENDPOINT; \
    } \
    M[i][j].trace = trace;

#define SELECT_TRACE_GOTOH_LOCAL_GAP(matrix, score1, score2, score3) \
    trace = M_MATRIX; \
    score = score1; \
    temp = score2; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = Ix_MATRIX; \
    } \
    else if (temp > score - epsilon) trace |= Ix_MATRIX; \
    temp = score3; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = Iy_MATRIX; \
    } \
    else if (temp > score - epsilon) trace |= Iy_MATRIX; \
    if (score < epsilon) { \
        score = -DBL_MAX; \
        trace = 0; \
    } \
    gaps[i][j].matrix = trace;

#define SELECT_TRACE_WATERMAN_SMITH_BEYER_GLOBAL_ALIGN(score4) \
    trace = M_MATRIX; \
    score = M_scores[i-1][j-1]; \
    temp = Ix_scores[i-1][j-1]; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = Ix_MATRIX; \
    } \
    else if (temp > score - epsilon) trace |= Ix_MATRIX; \
    temp = Iy_scores[i-1][j-1]; \
    if (temp > score + epsilon) { \
        score = temp; \
        trace = Iy_MATRIX; \
    } \
    else if (temp > score - epsilon) trace |= Iy_MATRIX; \
    M_scores[i][j] = score + score4; \
    M[i][j].trace = trace;

#define SELECT_TRACE_WATERMAN_SMITH_BEYER_GAP(score1, score2) \
    temp = score1 + gapscore; \
    if (temp > score - epsilon) { \
        if (temp > score + epsilon) { \
            score = temp; \
            nm = 0; \
            ng = 0; \
        } \
        gapM[nm] = gap; \
        nm++; \
    } \
    temp = score2 + gapscore; \
    if (temp > score - epsilon) { \
        if (temp > score + epsilon) { \
            score = temp; \
            nm = 0; \
            ng = 0; \
        } \
        gapXY[ng] = gap; \
        ng++; \
    }

#define SELECT_TRACE_WATERMAN_SMITH_BEYER_ALIGN(score1, score2, score3, score4) \
    trace = M_MATRIX; \
    score = score1; \
    if (score2 > score + epsilon) { \
        score = score2; \
        trace = Ix_MATRIX; \
    } \
    else if (score2 > score - epsilon) trace |= Ix_MATRIX; \
    if (score3 > score + epsilon) { \
        score = score3; \
        trace = Iy_MATRIX; \
    } \
    else if (score3 > score - epsilon) trace |= Iy_MATRIX; \
    score += score4; \
    if (score < epsilon) { \
        score = 0; \
        trace = STARTPOINT; \
    } \
    else if (score > maximum - epsilon) { \
        if (score > maximum + epsilon) { \
            maximum = score; \
            for ( ; im < i; im++, jm = 0) \
                for ( ; jm <= nB; jm++) M[im][jm].trace &= ~ENDPOINT; \
            for ( ; jm < j; jm++) M[im][jm].trace &= ~ENDPOINT; \
            im = i; \
            jm = j; \
        } \
        trace |= ENDPOINT; \
    } \
    M_scores[i][j] = score; \
    M[i][j].trace = trace;

/* -------------- allocation & deallocation ------------- */

static PathGenerator*
PathGenerator_create_NWSW(Py_ssize_t nA, Py_ssize_t nB, Mode mode)
{
    int i;
    unsigned char trace = 0;
    Trace** M;
    PathGenerator* paths;

    paths = (PathGenerator*)PyType_GenericAlloc(&PathGenerator_Type, 0);
    if (!paths) return NULL;

    paths->iA = 0;
    paths->iB = 0;
    paths->nA = nA;
    paths->nB = nB;
    paths->M = NULL;
    paths->gaps.gotoh = NULL;
    paths->gaps.waterman_smith_beyer = NULL;
    paths->algorithm = NeedlemanWunschSmithWaterman;
    paths->mode = mode;
    paths->length = 0;

    M = PyMem_Malloc((nA+1)*sizeof(Trace*));
    paths->M = M;
    if (!M) goto exit;
    switch (mode) {
        case Global: trace = VERTICAL; break;
        case Local: trace = STARTPOINT; break;
    }
    for (i = 0; i <= nA; i++) {
        M[i] = PyMem_Malloc((nB+1)*sizeof(Trace));
        if (!M[i]) goto exit;
        M[i][0].trace = trace;
    }
    if (mode == Global) {
        M[0][0].trace = 0;
        trace = HORIZONTAL;
    }
    for (i = 1; i <= nB; i++) M[0][i].trace = trace;
    M[0][0].path = 0;
    return paths;
exit:
    Py_DECREF(paths);
    PyErr_SetNone(PyExc_MemoryError);
    return NULL;
}

static PathGenerator*
PathGenerator_create_Gotoh(Py_ssize_t nA, Py_ssize_t nB, Mode mode)
{
    int i;
    unsigned char trace;
    Trace** M;
    TraceGapsGotoh** gaps;
    PathGenerator* paths;

    paths = (PathGenerator*)PyType_GenericAlloc(&PathGenerator_Type, 0);
    if (!paths) return NULL;

    paths->iA = 0;
    paths->iB = 0;
    paths->nA = nA;
    paths->nB = nB;
    paths->M = NULL;
    paths->gaps.gotoh = NULL;
    paths->algorithm = Gotoh;
    paths->mode = mode;
    paths->length = 0;

    switch (mode) {
        case Global: trace = 0; break;
        case Local: trace = STARTPOINT; break;
    }

    M = PyMem_Malloc((nA+1)*sizeof(Trace*));
    if (!M) goto exit;
    paths->M = M;
    for (i = 0; i <= nA; i++) {
        M[i] = PyMem_Malloc((nB+1)*sizeof(Trace));
        if (!M[i]) goto exit;
        M[i][0].trace = trace;
    }
    gaps = PyMem_Malloc((nA+1)*sizeof(TraceGapsGotoh*));
    if (!gaps) goto exit;
    paths->gaps.gotoh = gaps;
    for (i = 0; i <= nA; i++) {
        gaps[i] = PyMem_Malloc((nB+1)*sizeof(TraceGapsGotoh));
        if (!gaps[i]) goto exit;
    }

    gaps[0][0].Ix = 0;
    gaps[0][0].Iy = 0;
    if (mode == Global) {
        for (i = 1; i <= nA; i++) {
            gaps[i][0].Ix = Ix_MATRIX;
            gaps[i][0].Iy = 0;
        }
        gaps[1][0].Ix = M_MATRIX;
        for (i = 1; i <= nB; i++) {
            M[0][i].trace = 0;
            gaps[0][i].Ix = 0;
            gaps[0][i].Iy = Iy_MATRIX;
        }
        gaps[0][1].Iy = M_MATRIX;
    }
    else if (mode == Local) {
        for (i = 1; i < nA; i++) {
            gaps[i][0].Ix = 0;
            gaps[i][0].Iy = 0;
        }
        for (i = 1; i <= nB; i++) {
            M[0][i].trace = trace;
            gaps[0][i].Ix = 0;
            gaps[0][i].Iy = 0;
        }
    }
    M[0][0].path = 0;

    return paths;
exit:
    Py_DECREF(paths);
    PyErr_SetNone(PyExc_MemoryError);
    return NULL;
}

static PathGenerator*
PathGenerator_create_WSB(Py_ssize_t nA, Py_ssize_t nB, Mode mode)
{
    int i, j;
    int* trace;
    Trace** M = NULL;
    TraceGapsWatermanSmithBeyer** gaps = NULL;
    PathGenerator* paths;

    paths = (PathGenerator*)PyType_GenericAlloc(&PathGenerator_Type, 0);
    if (!paths) return NULL;

    paths->iA = 0;
    paths->iB = 0;
    paths->nA = nA;
    paths->nB = nB;
    paths->M = NULL;
    paths->gaps.waterman_smith_beyer = NULL;
    paths->algorithm = WatermanSmithBeyer;
    paths->mode = mode;
    paths->length = 0;

    M = PyMem_Malloc((nA+1)*sizeof(Trace*));
    if (!M) goto exit;
    paths->M = M;
    for (i = 0; i <= nA; i++) {
        M[i] = PyMem_Malloc((nB+1)*sizeof(Trace));
        if (!M[i]) goto exit;
    }
    gaps = PyMem_Malloc((nA+1)*sizeof(TraceGapsWatermanSmithBeyer*));
    if (!gaps) goto exit;
    paths->gaps.waterman_smith_beyer = gaps;
    for (i = 0; i <= nA; i++) gaps[i] = NULL;
    for (i = 0; i <= nA; i++) {
        gaps[i] = PyMem_Malloc((nB+1)*sizeof(TraceGapsWatermanSmithBeyer));
        if (!gaps[i]) goto exit;
        for (j = 0; j <= nB; j++) {
            gaps[i][j].MIx = NULL;
            gaps[i][j].IyIx = NULL;
            gaps[i][j].MIy = NULL;
            gaps[i][j].IxIy = NULL;
        }
        M[i][0].path = 0;
        switch (mode) {
            case Global:
                M[i][0].trace = 0;
                trace = PyMem_Malloc(2*sizeof(int));
                if (!trace) goto exit;
                gaps[i][0].MIx = trace;
                trace[0] = i;
                trace[1] = 0;
                trace = PyMem_Malloc(sizeof(int));
                if (!trace) goto exit;
                gaps[i][0].IyIx = trace;
                trace[0] = 0;
                break;
            case Local:
                M[i][0].trace = STARTPOINT;
                break;
        }
    }
    for (i = 1; i <= nB; i++) {
        switch (mode) {
            case Global:
                M[0][i].trace = 0;
                trace = PyMem_Malloc(2*sizeof(int));
                if (!trace) goto exit;
                gaps[0][i].MIy = trace;
                trace[0] = i;
                trace[1] = 0;
                trace = PyMem_Malloc(sizeof(int));
                if (!trace) goto exit;
                gaps[0][i].IxIy = trace;
                trace[0] = 0;
                break;
            case Local:
                M[0][i].trace = STARTPOINT;
                break;
        }
    }
    M[0][0].path = 0;
    return paths;
exit:
    Py_DECREF(paths);
    PyErr_SetNone(PyExc_MemoryError);
    return NULL;
}

/* ----------------- alignment algorithms ----------------- */

static PyObject*
Aligner_needlemanwunsch_score(Aligner* self, const char* sA, Py_ssize_t nA,
                                             const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int kA;
    int kB;
    const double gap_extend_A = self->target_extend_gap_score;
    const double gap_extend_B = self->query_extend_gap_score;
    const double left_gap_extend_A = self->target_left_extend_gap_score;
    const double right_gap_extend_A = self->target_right_extend_gap_score;
    const double left_gap_extend_B = self->query_left_extend_gap_score;
    const double right_gap_extend_B = self->query_right_extend_gap_score;
    double score;
    double temp;

    double* scores;

    /* Needleman-Wunsch algorithm */
    scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!scores) return PyErr_NoMemory();

    /* The top row of the score matrix is a special case,
     * as there are no previously aligned characters.
     */
    scores[0] = 0.0;
    for (j = 1; j <= nB; j++) scores[j] = j * left_gap_extend_A;
    for (i = 1; i < nA; i++) {
        kA = CHARINDEX(sA[i-1]);
        temp = scores[0];
        scores[0] = i * left_gap_extend_B;
        for (j = 1; j < nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_SCORE_GLOBAL(temp + self->substitution_matrix[kA][kB],
                                scores[j] + gap_extend_B,
                                scores[j-1] + gap_extend_A);
            temp = scores[j];
            scores[j] = score;
        }
        kB = CHARINDEX(sB[nB-1]);
        SELECT_SCORE_GLOBAL(temp + self->substitution_matrix[kA][kB],
                            scores[nB] + right_gap_extend_B,
                            scores[nB-1] + gap_extend_A);
        temp = scores[nB];
        scores[nB] = score;
    }
    kA = CHARINDEX(sA[nA-1]);
    temp = scores[0];
    scores[0] = nA * right_gap_extend_B;
    for (j = 1; j < nB; j++) {
        kB = CHARINDEX(sB[j-1]);
        SELECT_SCORE_GLOBAL(temp + self->substitution_matrix[kA][kB],
                            scores[j] + gap_extend_B,
                            scores[j-1] + right_gap_extend_A);
        temp = scores[j];
        scores[j] = score;
    }
    kB = CHARINDEX(sB[nB-1]);
    SELECT_SCORE_GLOBAL(temp + self->substitution_matrix[kA][kB],
                        scores[nB] + right_gap_extend_B,
                        scores[nB-1] + right_gap_extend_A);
    PyMem_Free(scores);
    return PyFloat_FromDouble(score);
}

static PyObject*
Aligner_smithwaterman_score(Aligner* self, const char* sA, Py_ssize_t nA,
                                           const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int kA;
    int kB;
    const double gap_extend_A = self->target_extend_gap_score;
    const double gap_extend_B = self->query_extend_gap_score;
    double score;
    double* scores;
    double temp;
    double maximum = 0;

    /* Smith-Waterman algorithm */
    scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!scores) return PyErr_NoMemory();

    /* The top row of the score matrix is a special case,
     * as there are no previously aligned characters.
     */
    for (j = 0; j <= nB; j++)
        scores[j] = 0;
    for (i = 1; i < nA; i++) {
        kA = CHARINDEX(sA[i-1]);
        temp = 0;
        for (j = 1; j < nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_SCORE_LOCAL3(temp + self->substitution_matrix[kA][kB],
                                scores[j] + gap_extend_B,
                                scores[j-1] + gap_extend_A);
            temp = scores[j];
            scores[j] = score;
        }
        kB = CHARINDEX(sB[nB-1]);
        SELECT_SCORE_LOCAL1(temp + self->substitution_matrix[kA][kB]);
        temp = scores[nB];
        scores[nB] = score;
    }
    kA = CHARINDEX(sA[nA-1]);
    temp = 0;
    for (j = 1; j < nB; j++) {
        kB = CHARINDEX(sB[j-1]);
        SELECT_SCORE_LOCAL1(temp + self->substitution_matrix[kA][kB]);
        temp = scores[j];
        scores[j] = score;
    }
    kB = CHARINDEX(sB[nB-1]);
    SELECT_SCORE_LOCAL1(temp + self->substitution_matrix[kA][kB]);
    PyMem_Free(scores);
    return PyFloat_FromDouble(maximum);
}

static PyObject*
Aligner_needlemanwunsch_align(Aligner* self, const char* sA, Py_ssize_t nA,
                                             const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int kA;
    int kB;
    const double gap_extend_A = self->target_extend_gap_score;
    const double gap_extend_B = self->query_extend_gap_score;
    const double left_gap_extend_A = self->target_left_extend_gap_score;
    const double left_gap_extend_B = self->query_left_extend_gap_score;
    const double right_gap_extend_A = self->target_right_extend_gap_score;
    const double right_gap_extend_B = self->query_right_extend_gap_score;
    const double epsilon = self->epsilon;
    Trace** M;
    double score;
    int trace;
    double temp;
    double* scores = NULL;
    PathGenerator* paths;

    /* Needleman-Wunsch algorithm */
    paths = PathGenerator_create_NWSW(nA, nB, Global);
    if (!paths) return NULL;
    scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!scores) {
        Py_DECREF(paths);
        return PyErr_NoMemory();
    }
    M = paths->M;
    scores[0] = 0;
    for (j = 1; j <= nB; j++) scores[j] = j * left_gap_extend_A;
    for (i = 1; i < nA; i++) {
        temp = scores[0];
        scores[0] = i * left_gap_extend_B;
        kA = CHARINDEX(sA[i-1]);
        for (j = 1; j < nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_TRACE_NEEDLEMAN_WUNSCH(gap_extend_A, gap_extend_B);
        }
        kB = CHARINDEX(sB[j-1]);
        SELECT_TRACE_NEEDLEMAN_WUNSCH(gap_extend_A, right_gap_extend_B);
    }
    temp = scores[0];
    scores[0] = i * left_gap_extend_B;
    kA = CHARINDEX(sA[nA-1]);
    for (j = 1; j < nB; j++) {
        kB = CHARINDEX(sB[j-1]);
        SELECT_TRACE_NEEDLEMAN_WUNSCH(right_gap_extend_A, gap_extend_B);
    }
    kB = CHARINDEX(sB[j-1]);
    SELECT_TRACE_NEEDLEMAN_WUNSCH(right_gap_extend_A, right_gap_extend_B);
    PyMem_Free(scores);
    M[nA][nB].path = 0;

    return Py_BuildValue("fN", score, paths);
}

static PyObject*
Aligner_smithwaterman_align(Aligner* self, const char* sA, Py_ssize_t nA,
                                           const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int im = nA;
    int jm = nB;
    int kA;
    int kB;
    const double gap_extend_A = self->target_extend_gap_score;
    const double gap_extend_B = self->query_extend_gap_score;
    const double epsilon = self->epsilon;
    Trace** M = NULL;
    double maximum = 0;
    double score = 0;
    double* scores = NULL;
    double temp;
    int trace;
    PathGenerator* paths = NULL;

    /* Smith-Waterman algorithm */
    paths = PathGenerator_create_NWSW(nA, nB, Local);
    if (!paths) return NULL;
    scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!scores) {
        Py_DECREF(paths);
        return PyErr_NoMemory();
    }
    M = paths->M;
    for (j = 0; j <= nB; j++) scores[j] = 0;
    for (i = 1; i < nA; i++) {
        temp = 0;
        kA = CHARINDEX(sA[i-1]);
        for (j = 1; j < nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_TRACE_SMITH_WATERMAN_HVD(gap_extend_A, gap_extend_B);
        }
        kB = CHARINDEX(sB[nB-1]);
        SELECT_TRACE_SMITH_WATERMAN_D;
    }
    temp = 0;
    kA = CHARINDEX(sA[nA-1]);
    for (j = 1; j < nB; j++) {
        kB = CHARINDEX(sB[j-1]);
        SELECT_TRACE_SMITH_WATERMAN_D;
    }
    kB = CHARINDEX(sB[nB-1]);
    SELECT_TRACE_SMITH_WATERMAN_D;
    PyMem_Free(scores);

    /* As we don't allow zero-score extensions to alignments,
     * we need to remove all traces towards an ENDPOINT.
     * In addition, some points then won't have any path to a STARTPOINT.
     * Here, use path as a temporary variable to indicate if the point
     * is reachable from a STARTPOINT. If it is unreachable, remove all
     * traces from it, and don't allow it to be an ENDPOINT. It may still
     * be a valid STARTPOINT. */
    for (j = 0; j <= nB; j++) M[0][j].path = 1;
    for (i = 1; i <= nA; i++) {
        M[i][0].path = 1;
        for (j = 1; j <= nB; j++) {
            trace = M[i][j].trace;
            /* Remove traces to unreachable points. */
            if (!M[i-1][j-1].path) trace &= ~DIAGONAL;
            if (!M[i][j-1].path) trace &= ~HORIZONTAL;
            if (!M[i-1][j].path) trace &= ~VERTICAL;
            if (trace & (STARTPOINT | HORIZONTAL | VERTICAL | DIAGONAL)) {
                /* The point is reachable. */
                if (trace & ENDPOINT) M[i][j].path = 0; /* no extensions after ENDPOINT */
                else M[i][j].path = 1;
            }
            else {
                /* The point is not reachable. Then it is not a STARTPOINT,
                 * all traces from it can be removed, and it cannot act as
                 * an ENDPOINT. */
                M[i][j].path = 0;
                trace = 0;
            }
            M[i][j].trace = trace;
        }
    }

    if (maximum == 0) M[0][0].path = NONE;
    else M[0][0].path = 0;

    return Py_BuildValue("fN", maximum, paths);
}

static PyObject*
Aligner_gotoh_global_score(Aligner* self, const char* sA, Py_ssize_t nA,
                                          const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int kA;
    int kB;
    const double gap_open_A = self->target_open_gap_score;
    const double gap_open_B = self->query_open_gap_score;
    const double gap_extend_A = self->target_extend_gap_score;
    const double gap_extend_B = self->query_extend_gap_score;
    const double left_gap_open_A = self->target_left_open_gap_score;
    const double left_gap_open_B = self->query_left_open_gap_score;
    const double left_gap_extend_A = self->target_left_extend_gap_score;
    const double left_gap_extend_B = self->query_left_extend_gap_score;
    const double right_gap_open_A = self->target_right_open_gap_score;
    const double right_gap_open_B = self->query_right_open_gap_score;
    const double right_gap_extend_A = self->target_right_extend_gap_score;
    const double right_gap_extend_B = self->query_right_extend_gap_score;
    double* M_scores = NULL;
    double* Ix_scores = NULL;
    double* Iy_scores = NULL;
    double score;
    double temp;
    double M_temp;
    double Ix_temp;
    double Iy_temp;

    /* Gotoh algorithm with three states */
    M_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!M_scores) goto exit;
    Ix_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!Ix_scores) goto exit;
    Iy_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!Iy_scores) goto exit;

    /* The top row of the score matrix is a special case,
     * as there are no previously aligned characters.
     */
    M_scores[0] = 0;
    Ix_scores[0] = -DBL_MAX;
    Iy_scores[0] = -DBL_MAX;
    for (j = 1; j <= nB; j++) {
        M_scores[j] = -DBL_MAX;
        Ix_scores[j] = -DBL_MAX;
        Iy_scores[j] = left_gap_open_A + left_gap_extend_A * (j-1);
    }

    for (i = 1; i < nA; i++) {
        M_temp = M_scores[0];
        Ix_temp = Ix_scores[0];
        Iy_temp = Iy_scores[0];
        M_scores[0] = -DBL_MAX;
        Ix_scores[0] = left_gap_open_B + left_gap_extend_B * (i-1);
        Iy_scores[0] = -DBL_MAX;
        kA = CHARINDEX(sA[i-1]);
        for (j = 1; j < nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_SCORE_GLOBAL(M_temp,
                                Ix_temp,
                                Iy_temp);
            M_temp = M_scores[j];
            M_scores[j] = score + self->substitution_matrix[kA][kB];
            SELECT_SCORE_GLOBAL(M_temp + gap_open_B,
                                Ix_scores[j] + gap_extend_B,
                                Iy_scores[j] + gap_open_B);
            Ix_temp = Ix_scores[j];
            Ix_scores[j] = score;
            SELECT_SCORE_GLOBAL(M_scores[j-1] + gap_open_A,
                                Ix_scores[j-1] + gap_open_A,
                                Iy_scores[j-1] + gap_extend_A);
            Iy_temp = Iy_scores[j];
            Iy_scores[j] = score;
        }
        kB = CHARINDEX(sB[nB-1]);
        SELECT_SCORE_GLOBAL(M_temp,
                            Ix_temp,
                            Iy_temp);
        M_temp = M_scores[nB];
        M_scores[nB] = score + self->substitution_matrix[kA][kB];
        SELECT_SCORE_GLOBAL(M_temp + right_gap_open_B,
                            Ix_scores[nB] + right_gap_extend_B,
                            Iy_scores[nB] + right_gap_open_B);
        Ix_scores[nB] = score;
        SELECT_SCORE_GLOBAL(M_scores[nB-1] + gap_open_A,
                            Iy_scores[nB-1] + gap_extend_A,
                            Ix_scores[nB-1] + gap_open_A);
        Iy_scores[nB] = score;
    }

    M_temp = M_scores[0];
    Ix_temp = Ix_scores[0];
    Iy_temp = Iy_scores[0];
    M_scores[0] = -DBL_MAX;
    Ix_scores[0] = left_gap_open_B + left_gap_extend_B * (i-1);
    Iy_scores[0] = -DBL_MAX;
    kA = CHARINDEX(sA[nA-1]);
    for (j = 1; j < nB; j++) {
        kB = CHARINDEX(sB[j-1]);
        SELECT_SCORE_GLOBAL(M_temp,
                            Ix_temp,
                            Iy_temp);
        M_temp = M_scores[j];
        M_scores[j] = score + self->substitution_matrix[kA][kB];
        SELECT_SCORE_GLOBAL(M_temp + gap_open_B,
                            Ix_scores[j] + gap_extend_B,
                            Iy_scores[j] + gap_open_B);
        Ix_temp = Ix_scores[j];
        Ix_scores[j] = score;
        SELECT_SCORE_GLOBAL(M_scores[j-1] + right_gap_open_A,
                            Iy_scores[j-1] + right_gap_extend_A,
                            Ix_scores[j-1] + right_gap_open_A);
        Iy_temp = Iy_scores[j];
        Iy_scores[j] = score;
    }

    kB = CHARINDEX(sB[nB-1]);
    SELECT_SCORE_GLOBAL(M_temp,
                        Ix_temp,
                        Iy_temp);
    M_temp = M_scores[nB];
    M_scores[nB] = score + self->substitution_matrix[kA][kB];
    SELECT_SCORE_GLOBAL(M_temp + right_gap_open_B,
                        Ix_scores[nB] + right_gap_extend_B,
                        Iy_scores[nB] + right_gap_open_B);
    Ix_temp = Ix_scores[nB];
    Ix_scores[nB] = score;
    SELECT_SCORE_GLOBAL(M_scores[nB-1] + right_gap_open_A,
                        Ix_scores[nB-1] + right_gap_open_A,
                        Iy_scores[nB-1] + right_gap_extend_A);
    Iy_temp = Iy_scores[nB];
    Iy_scores[nB] = score;

    SELECT_SCORE_GLOBAL(M_scores[nB], Ix_scores[nB], Iy_scores[nB]);
    PyMem_Free(M_scores);
    PyMem_Free(Ix_scores);
    PyMem_Free(Iy_scores);
    return PyFloat_FromDouble(score);

exit:
    if (M_scores) PyMem_Free(M_scores);
    if (Ix_scores) PyMem_Free(Ix_scores);
    if (Iy_scores) PyMem_Free(Iy_scores);
    return PyErr_NoMemory();
}

static PyObject*
Aligner_gotoh_local_score(Aligner* self, const char* sA, Py_ssize_t nA,
                                         const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int kA;
    int kB;
    const double gap_open_A = self->target_open_gap_score;
    const double gap_open_B = self->query_open_gap_score;
    const double gap_extend_A = self->target_extend_gap_score;
    const double gap_extend_B = self->query_extend_gap_score;
    double* M_scores = NULL;
    double* Ix_scores = NULL;
    double* Iy_scores = NULL;
    double score;
    double temp;
    double M_temp;
    double Ix_temp;
    double Iy_temp;
    double maximum = 0.0;

    /* Gotoh algorithm with three states */
    M_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!M_scores) goto exit;
    Ix_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!Ix_scores) goto exit;
    Iy_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!Iy_scores) goto exit;

    /* The top row of the score matrix is a special case,
     * as there are no previously aligned characters.
     */
    M_scores[0] = 0;
    Ix_scores[0] = -DBL_MAX;
    Iy_scores[0] = -DBL_MAX;
    for (j = 1; j <= nB; j++) {
        M_scores[j] = -DBL_MAX;
        Ix_scores[j] = -DBL_MAX;
        Iy_scores[j] = 0;
    }

    for (i = 1; i < nA; i++) {
        M_temp = M_scores[0];
        Ix_temp = Ix_scores[0];
        Iy_temp = Iy_scores[0];
        M_scores[0] = -DBL_MAX;
        Ix_scores[0] = 0;
        Iy_scores[0] = -DBL_MAX;
        kA = CHARINDEX(sA[i-1]);
        for (j = 1; j < nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_SCORE_GOTOH_LOCAL_ALIGN(M_temp,
                                           Ix_temp,
                                           Iy_temp,
                                           self->substitution_matrix[kA][kB]);
            M_temp = M_scores[j];
            M_scores[j] = score;
            SELECT_SCORE_LOCAL3(M_temp + gap_open_B,
                                Ix_scores[j] + gap_extend_B,
                                Iy_scores[j] + gap_open_B);
            Ix_temp = Ix_scores[j];
            Ix_scores[j] = score;
            SELECT_SCORE_LOCAL3(M_scores[j-1] + gap_open_A,
                                Ix_scores[j-1] + gap_open_A,
                                Iy_scores[j-1] + gap_extend_A);
            Iy_temp = Iy_scores[j];
            Iy_scores[j] = score;
        }

        kB = CHARINDEX(sB[nB-1]);

        Ix_scores[nB] = 0;
        Iy_scores[nB] = 0;
        SELECT_SCORE_GOTOH_LOCAL_ALIGN(M_temp,
                                       Ix_temp,
                                       Iy_temp,
                                       self->substitution_matrix[kA][kB]);
        M_temp = M_scores[nB];
        M_scores[nB] = score;
    }

    M_temp = M_scores[0];
    Ix_temp = Ix_scores[0];
    Iy_temp = Iy_scores[0];
    M_scores[0] = -DBL_MAX;
    Ix_scores[0] = 0;
    Iy_scores[0] = -DBL_MAX;
    kA = CHARINDEX(sA[nA-1]);
    for (j = 1; j < nB; j++) {
        kB = CHARINDEX(sB[j-1]);
        SELECT_SCORE_GOTOH_LOCAL_ALIGN(M_temp,
                                       Ix_temp,
                                       Iy_temp,
                                       self->substitution_matrix[kA][kB]);
        M_temp = M_scores[j];
        M_scores[j] = score;
        Ix_temp = Ix_scores[j];
        Iy_temp = Iy_scores[j];
        Ix_scores[j] = 0;
        Iy_scores[j] = 0;
    }

    kB = CHARINDEX(sB[nB-1]);
    SELECT_SCORE_GOTOH_LOCAL_ALIGN(M_temp,
                                   Ix_temp,
                                   Iy_temp,
                                   self->substitution_matrix[kA][kB]);

    PyMem_Free(M_scores);
    PyMem_Free(Ix_scores);
    PyMem_Free(Iy_scores);
    return PyFloat_FromDouble(maximum);

exit:
    if (M_scores) PyMem_Free(M_scores);
    if (Ix_scores) PyMem_Free(Ix_scores);
    if (Iy_scores) PyMem_Free(Iy_scores);
    return PyErr_NoMemory();
}

static PyObject*
Aligner_gotoh_global_align(Aligner* self, const char* sA, Py_ssize_t nA,
                                          const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int kA;
    int kB;
    const double gap_open_A = self->target_open_gap_score;
    const double gap_open_B = self->query_open_gap_score;
    const double gap_extend_A = self->target_extend_gap_score;
    const double gap_extend_B = self->query_extend_gap_score;
    const double left_gap_open_A = self->target_left_open_gap_score;
    const double left_gap_open_B = self->query_left_open_gap_score;
    const double left_gap_extend_A = self->target_left_extend_gap_score;
    const double left_gap_extend_B = self->query_left_extend_gap_score;
    const double right_gap_open_A = self->target_right_open_gap_score;
    const double right_gap_open_B = self->query_right_open_gap_score;
    const double right_gap_extend_A = self->target_right_extend_gap_score;
    const double right_gap_extend_B = self->query_right_extend_gap_score;
    const double epsilon = self->epsilon;
    TraceGapsGotoh** gaps = NULL;
    Trace** M = NULL;
    double* M_scores = NULL;
    double* Ix_scores = NULL;
    double* Iy_scores = NULL;
    double score;
    int trace;
    double temp;
    double M_temp;
    double Ix_temp;
    double Iy_temp;
    PathGenerator* paths;

    /* Gotoh algorithm with three states */
    paths = PathGenerator_create_Gotoh(nA, nB, Global);
    if (!paths) return NULL;

    M_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!M_scores) goto exit;
    Ix_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!Ix_scores) goto exit;
    Iy_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!Iy_scores) goto exit;
    M = paths->M;
    gaps = paths->gaps.gotoh;

    /* Gotoh algorithm with three states */
    M_scores[0] = 0;
    Ix_scores[0] = -DBL_MAX;
    Iy_scores[0] = -DBL_MAX;

    for (j = 1; j <= nB; j++) {
        M_scores[j] = -DBL_MAX;
        Ix_scores[j] = -DBL_MAX;
        Iy_scores[j] = left_gap_open_A + left_gap_extend_A * (j-1);
    }

    for (i = 1; i < nA; i++) {
        kA = CHARINDEX(sA[i-1]);
        M_temp = M_scores[0];
        Ix_temp = Ix_scores[0];
        Iy_temp = Iy_scores[0];
        M_scores[0] = -DBL_MAX;
        Ix_scores[0] = left_gap_open_B + left_gap_extend_B * (i-1);
        Iy_scores[0] = -DBL_MAX;
        for (j = 1; j < nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_TRACE_GOTOH_GLOBAL_ALIGN;
            M_temp = M_scores[j];
            M_scores[j] = score + self->substitution_matrix[kA][kB];
            SELECT_TRACE_GOTOH_GLOBAL_GAP(Ix,
                                          M_temp + gap_open_B,
                                          Ix_scores[j] + gap_extend_B,
                                          Iy_scores[j] + gap_open_B);
            Ix_temp = Ix_scores[j];
            Ix_scores[j] = score;
            SELECT_TRACE_GOTOH_GLOBAL_GAP(Iy,
                                          M_scores[j-1] + gap_open_A,
                                          Ix_scores[j-1] + gap_open_A,
                                          Iy_scores[j-1] + gap_extend_A);
            Iy_temp = Iy_scores[j];
            Iy_scores[j] = score;
        }
        kB = CHARINDEX(sB[nB-1]);
        SELECT_TRACE_GOTOH_GLOBAL_ALIGN;
        M_temp = M_scores[nB];
        M_scores[nB] = score + self->substitution_matrix[kA][kB];
        SELECT_TRACE_GOTOH_GLOBAL_GAP(Ix,
                                      M_temp + right_gap_open_B,
                                      Ix_scores[nB] + right_gap_extend_B,
                                      Iy_scores[nB] + right_gap_open_B);
        Ix_temp = Ix_scores[nB];
        Ix_scores[nB] = score;
        SELECT_TRACE_GOTOH_GLOBAL_GAP(Iy,
                                      M_scores[nB-1] + gap_open_A,
                                      Ix_scores[nB-1] + gap_open_A,
                                      Iy_scores[nB-1] + gap_extend_A);
        Iy_temp = Iy_scores[nB];
        Iy_scores[nB] = score;
    }
    kA = CHARINDEX(sA[nA-1]);
    M_temp = M_scores[0];
    Ix_temp = Ix_scores[0];
    Iy_temp = Iy_scores[0];
    M_scores[0] = -DBL_MAX;
    Ix_scores[0] = left_gap_open_B + left_gap_extend_B * (nA-1);
    Iy_scores[0] = -DBL_MAX;
    for (j = 1; j < nB; j++) {
        kB = CHARINDEX(sB[j-1]);
        SELECT_TRACE_GOTOH_GLOBAL_ALIGN;
        M_temp = M_scores[j];
        M_scores[j] = score + self->substitution_matrix[kA][kB];
        SELECT_TRACE_GOTOH_GLOBAL_GAP(Ix,
                                      M_temp + gap_open_B,
                                      Ix_scores[j] + gap_extend_B,
                                      Iy_scores[j] + gap_open_B);
        Ix_temp = Ix_scores[j];
        Ix_scores[j] = score;
        SELECT_TRACE_GOTOH_GLOBAL_GAP(Iy,
                                      M_scores[j-1] + right_gap_open_A,
                                      Ix_scores[j-1] + right_gap_open_A,
                                      Iy_scores[j-1] + right_gap_extend_A);
        Iy_temp = Iy_scores[j];
        Iy_scores[j] = score;
    }
    kB = CHARINDEX(sB[nB-1]);
    SELECT_TRACE_GOTOH_GLOBAL_ALIGN;
    M_temp = M_scores[j];
    M_scores[j] = score + self->substitution_matrix[kA][kB];
    SELECT_TRACE_GOTOH_GLOBAL_GAP(Ix,
                                  M_temp + right_gap_open_B,
                                  Ix_scores[j] + right_gap_extend_B,
                                  Iy_scores[j] + right_gap_open_B);
    Ix_scores[nB] = score;
    SELECT_TRACE_GOTOH_GLOBAL_GAP(Iy,
                                  M_scores[j-1] + right_gap_open_A,
                                  Ix_scores[j-1] + right_gap_open_A,
                                  Iy_scores[j-1] + right_gap_extend_A);
    Iy_scores[nB] = score;
    M[nA][nB].path = 0;

    /* traceback */
    SELECT_SCORE_GLOBAL(M_scores[nB], Ix_scores[nB], Iy_scores[nB]);
    if (M_scores[nB] < score - epsilon) M[nA][nB].trace = 0;
    if (Ix_scores[nB] < score - epsilon) gaps[nA][nB].Ix = 0;
    if (Iy_scores[nB] < score - epsilon) gaps[nA][nB].Iy = 0;
    return Py_BuildValue("fN", score, paths);
exit:
    Py_DECREF(paths);
    if (M_scores) PyMem_Free(M_scores);
    if (Ix_scores) PyMem_Free(Ix_scores);
    if (Iy_scores) PyMem_Free(Iy_scores);
    return PyErr_NoMemory();
}

static PyObject*
Aligner_gotoh_local_align(Aligner* self, const char* sA, Py_ssize_t nA,
                                         const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int im = nA;
    int jm = nB;
    int kA;
    int kB;
    const double gap_open_A = self->target_open_gap_score;
    const double gap_open_B = self->query_open_gap_score;
    const double gap_extend_A = self->target_extend_gap_score;
    const double gap_extend_B = self->query_extend_gap_score;
    const double epsilon = self->epsilon;
    Trace** M = NULL;
    TraceGapsGotoh** gaps = NULL;
    double* M_scores = NULL;
    double* Ix_scores = NULL;
    double* Iy_scores = NULL;
    double score;
    int trace;
    double temp;
    double M_temp;
    double Ix_temp;
    double Iy_temp;
    double maximum = 0.0;
    PathGenerator* paths;

    /* Gotoh algorithm with three states */
    paths = PathGenerator_create_Gotoh(nA, nB, Local);
    if (!paths) return NULL;
    M = paths->M;
    gaps = paths->gaps.gotoh;

    M_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!M_scores) goto exit;
    Ix_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!Ix_scores) goto exit;
    Iy_scores = PyMem_Malloc((nB+1)*sizeof(double));
    if (!Iy_scores) goto exit;
    M_scores[0] = 0;
    Ix_scores[0] = -DBL_MAX;
    Iy_scores[0] = -DBL_MAX;

    for (j = 1; j <= nB; j++) {
        M_scores[j] = 0;
        Ix_scores[j] = -DBL_MAX;
        Iy_scores[j] = -DBL_MAX;
    }
    for (i = 1; i < nA; i++) {
        M_temp = M_scores[0];
        Ix_temp = Ix_scores[0];
        Iy_temp = Iy_scores[0];
        M_scores[0] = 0;
        Ix_scores[0] = -DBL_MAX;
        Iy_scores[0] = -DBL_MAX;
        kA = CHARINDEX(sA[i-1]);
        for (j = 1; j < nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_TRACE_GOTOH_LOCAL_ALIGN
            M_temp = M_scores[j];
            M_scores[j] = score;
            SELECT_TRACE_GOTOH_LOCAL_GAP(Ix,
                                     M_temp + gap_open_B,
                                     Ix_scores[j] + gap_extend_B,
                                     Iy_scores[j] + gap_open_B);
            Ix_temp = Ix_scores[j];
            Ix_scores[j] = score;
            SELECT_TRACE_GOTOH_LOCAL_GAP(Iy,
                                     M_scores[j-1] + gap_open_A,
                                     Ix_scores[j-1] + gap_open_A,
                                     Iy_scores[j-1] + gap_extend_A);
            Iy_temp = Iy_scores[j];
            Iy_scores[j] = score;
        }
        kB = CHARINDEX(sB[nB-1]);
        SELECT_TRACE_GOTOH_LOCAL_ALIGN
        M_temp = M_scores[j];
        M_scores[j] = score;
        Ix_temp = Ix_scores[nB];
        Ix_scores[nB] = 0;
        gaps[i][nB].Ix = 0;
        Iy_temp = Iy_scores[nB];
        Iy_scores[nB] = 0;
        gaps[i][nB].Iy = 0;
    }
    M_temp = M_scores[0];
    M_scores[0] = 0;
    M[nA][0].trace = 0;
    Ix_temp = Ix_scores[0];
    Ix_scores[0] = -DBL_MAX;
    gaps[nA][0].Ix = 0;
    gaps[nA][0].Iy = 0;
    Iy_temp = Iy_scores[0];
    Iy_scores[0] = -DBL_MAX;
    kA = CHARINDEX(sA[nA-1]);
    for (j = 1; j < nB; j++) {
        kB = CHARINDEX(sB[j-1]);
        SELECT_TRACE_GOTOH_LOCAL_ALIGN
        M_temp = M_scores[j];
        M_scores[j] = score;
        Ix_temp = Ix_scores[j];
        Ix_scores[j] = 0;
        gaps[nA][j].Ix = 0;
        Iy_temp = Iy_scores[j];
        Iy_scores[j] = 0;
        gaps[nA][j].Iy = 0;
    }
    kB = CHARINDEX(sB[nB-1]);
    SELECT_TRACE_GOTOH_LOCAL_ALIGN
    gaps[nA][nB].Ix = 0;
    gaps[nA][nB].Iy = 0;

    PyMem_Free(M_scores);
    PyMem_Free(Ix_scores);
    PyMem_Free(Iy_scores);

    /* As we don't allow zero-score extensions to alignments,
     * we need to remove all traces towards an ENDPOINT.
     * In addition, some points then won't have any path to a STARTPOINT.
     * Here, use path as a temporary variable to indicate if the point
     * is reachable from a STARTPOINT. If it is unreachable, remove all
     * traces from it, and don't allow it to be an ENDPOINT. It may still
     * be a valid STARTPOINT. */
    for (j = 0; j <= nB; j++) M[0][j].path = M_MATRIX;
    for (i = 1; i <= nA; i++) {
        M[i][0].path = M_MATRIX;
        for (j = 1; j <= nB; j++) {
            /* Remove traces to unreachable points. */
            trace = M[i][j].trace;
            if (!(M[i-1][j-1].path & M_MATRIX)) trace &= ~M_MATRIX;
            if (!(M[i-1][j-1].path & Ix_MATRIX)) trace &= ~Ix_MATRIX;
            if (!(M[i-1][j-1].path & Iy_MATRIX)) trace &= ~Iy_MATRIX;
            if (trace & (STARTPOINT | M_MATRIX | Ix_MATRIX | Iy_MATRIX)) {
                /* The point is reachable. */
                if (trace & ENDPOINT) M[i][j].path = 0; /* no extensions after ENDPOINT */
                else M[i][j].path |= M_MATRIX;
            }
            else {
                /* The point is not reachable. Then it is not a STARTPOINT,
                 * all traces from it can be removed, and it cannot act as
                 * an ENDPOINT. */
                M[i][j].path &= ~M_MATRIX;
                trace = 0;
            }
            M[i][j].trace = trace;
            trace = gaps[i][j].Ix;
            if (!(M[i-1][j].path & M_MATRIX)) trace &= ~M_MATRIX;
            if (!(M[i-1][j].path & Ix_MATRIX)) trace &= ~Ix_MATRIX;
            if (!(M[i-1][j].path & Iy_MATRIX)) trace &= ~Iy_MATRIX;
            if (trace & (M_MATRIX | Ix_MATRIX | Iy_MATRIX)) {
                /* The point is reachable. */
                M[i][j].path |= Ix_MATRIX;
            }
            else {
                /* The point is not reachable. Then
                 * all traces from it can be removed. */
                M[i][j].path &= ~Ix_MATRIX;
                trace = 0;
            }
            gaps[i][j].Ix = trace;
            trace = gaps[i][j].Iy;
            if (!(M[i][j-1].path & M_MATRIX)) trace &= ~M_MATRIX;
            if (!(M[i][j-1].path & Ix_MATRIX)) trace &= ~Ix_MATRIX;
            if (!(M[i][j-1].path & Iy_MATRIX)) trace &= ~Iy_MATRIX;
            if (trace & (M_MATRIX | Ix_MATRIX | Iy_MATRIX)) {
                /* The point is reachable. */
                M[i][j].path |= Iy_MATRIX;
            }
            else {
                /* The point is not reachable. Then
                 * all traces from it can be removed. */
                M[i][j].path &= ~Iy_MATRIX;
                trace = 0;
            }
            gaps[i][j].Iy = trace;
        }
    }

    /* traceback */
    if (maximum == 0) M[0][0].path = DONE;
    else M[0][0].path = 0;

    return Py_BuildValue("fN", maximum, paths);
exit:
    Py_DECREF(paths);
    if (M_scores) PyMem_Free(M_scores);
    if (Ix_scores) PyMem_Free(Ix_scores);
    if (Iy_scores) PyMem_Free(Iy_scores);
    return PyErr_NoMemory();
}

static int
_call_query_gap_function(Aligner* aligner, int i, int j, double* score)
{
    double value;
    PyObject* result;
    PyObject* function = aligner->query_gap_function;
    if (!function)
        value = aligner->query_open_gap_score
              + (j-1) * aligner->query_extend_gap_score;
    else {
        result = PyObject_CallFunction(function, "ii", i, j);
        if (result == NULL) return 0;
        value = PyFloat_AsDouble(result);
        Py_DECREF(result);
        if (value == -1.0 && PyErr_Occurred()) return 0;
    }
    *score = value;
    return 1;
}

static int
_call_target_gap_function(Aligner* aligner, int i, int j, double* score)
{
    double value;
    PyObject* result;
    PyObject* function = aligner->target_gap_function;
    if (!function)
        value = aligner->target_open_gap_score
              + (j-1) * aligner->target_extend_gap_score;
    else {
        result = PyObject_CallFunction(function, "ii", i, j);
        if (result == NULL) return 0;
        value = PyFloat_AsDouble(result);
        Py_DECREF(result);
        if (value == -1.0 && PyErr_Occurred()) return 0;
    }
    *score = value;
    return 1;
}

static PyObject*
Aligner_waterman_smith_beyer_global_score(Aligner* self,
                                          const char* sA, Py_ssize_t nA,
                                          const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int k;
    int kA;
    int kB;
    double** M = NULL;
    double** Ix = NULL;
    double** Iy = NULL;
    double score = 0.0;
    double gapscore;
    double temp;
    int ok = 1;

    /* Waterman-Smith-Beyer algorithm */
    M = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!M) goto exit;
    Ix = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!Ix) goto exit;
    Iy = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!Iy) goto exit;
    for (i = 0; i <= nA; i++) {
        M[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!M[i]) goto exit;
        Ix[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!Ix[i]) goto exit;
        Iy[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!Iy[i]) goto exit;
    }

    /* The top row of the score matrix is a special case,
     *  as there are no previously aligned characters.
     */
    M[0][0] = 0;
    Ix[0][0] = -DBL_MAX;
    Iy[0][0] = -DBL_MAX;
    for (i = 1; i <= nA; i++) {
        ok = _call_query_gap_function(self, 0, i, &score);
        if (!ok) goto exit;
        M[i][0] = -DBL_MAX;
        Ix[i][0] = score;
        Iy[i][0] = -DBL_MAX;
    }
    for (j = 1; j <= nB; j++) {
        ok = _call_target_gap_function(self, 0, j, &score);
        if (!ok) goto exit;
        M[0][j] = -DBL_MAX;
        Ix[0][j] = -DBL_MAX;
        Iy[0][j] = score;
    }
    for (i = 1; i <= nA; i++) {
        kA = CHARINDEX(sA[i-1]);
        for (j = 1; j <= nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_SCORE_GLOBAL(M[i-1][j-1], Ix[i-1][j-1], Iy[i-1][j-1]);
            M[i][j] = score + self->substitution_matrix[kA][kB];
            score = -DBL_MAX;
            for (k = 1; k <= i; k++) {
                ok = _call_query_gap_function(self, j, k, &gapscore);
                if (!ok) goto exit;
                SELECT_SCORE_WATERMAN_SMITH_BEYER(M[i-k][j], Iy[i-k][j]);
            }
            Ix[i][j] = score;
            score = -DBL_MAX;
            for (k = 1; k <= j; k++) {
                ok = _call_target_gap_function(self, i, k, &gapscore);
                if (!ok) goto exit;
                SELECT_SCORE_WATERMAN_SMITH_BEYER(M[i][j-k], Ix[i][j-k]);
            }
            Iy[i][j] = score;
        }
    }
    SELECT_SCORE_GLOBAL(M[nA][nB], Ix[nA][nB], Iy[nA][nB]);

exit:
    if (M) {
        /* If M is NULL, then Ix is also NULL. */
        if (Ix) {
            /* If Ix is NULL, then Iy is also NULL. */
            if (Iy) {
                /* If Iy is NULL, then M[i], Ix[i], and Iy[i] are also NULL. */ 
                for (i = 0; i <= nA; i++) {
                    if (!M[i]) break;
                    PyMem_Free(M[i]); 
                    if (!Ix[i]) break;
                    PyMem_Free(Ix[i]);
                    if (!Iy[i]) break;
                    PyMem_Free(Iy[i]);
                }
                PyMem_Free(Iy);
            }
            PyMem_Free(Ix);
        }
        PyMem_Free(M);
    }
    if (!ok) return NULL;
    return PyFloat_FromDouble(score);
}

static PyObject*
Aligner_waterman_smith_beyer_global_align(Aligner* self,
                                          const char* sA, Py_ssize_t nA,
                                          const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int gap;
    int kA;
    int kB;
    const double epsilon = self->epsilon;
    Trace** M;
    TraceGapsWatermanSmithBeyer** gaps;
    double** M_scores = NULL;
    double** Ix_scores = NULL;
    double** Iy_scores = NULL;
    int ng;
    int nm;
    double score;
    double gapscore;
    double temp;
    int trace;
    int* gapM;
    int* gapXY;
    int ok = 1;
    PathGenerator* paths = NULL;

    /* Waterman-Smith-Beyer algorithm */
    paths = PathGenerator_create_WSB(nA, nB, Global);
    if (!paths) return NULL;
    M = paths->M;
    gaps = paths->gaps.waterman_smith_beyer;

    M_scores = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!M_scores) goto exit;
    Ix_scores = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!Ix_scores) goto exit;
    Iy_scores = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!Iy_scores) goto exit;
    for (i = 0; i <= nA; i++) {
        M_scores[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!M_scores[i]) goto exit;
        M_scores[i][0] = -DBL_MAX;
        Ix_scores[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!Ix_scores[i]) goto exit;
        Ix_scores[i][0] = 0;
        Iy_scores[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!Iy_scores[i]) goto exit;
        Iy_scores[i][0] = -DBL_MAX;
    }
    M_scores[0][0] = 0;
    Ix_scores[0][0] = -DBL_MAX;
    for (i = 1; i <= nB; i++) {
        M_scores[0][i] = -DBL_MAX;
        Ix_scores[0][i] = -DBL_MAX;
        Iy_scores[0][i] = 0;
    }
    for (i = 1; i <= nA; i++) {
        ok = _call_query_gap_function(self, 0, i, &score);
        if (!ok) goto exit;
        Ix_scores[i][0] = score;
    }
    for (j = 1; j <= nB; j++) {
        ok = _call_target_gap_function(self, 0, j, &score);
        if (!ok) goto exit;
        Iy_scores[0][j] = score;
    }
    for (i = 1; i <= nA; i++) {
        kA = CHARINDEX(sA[i-1]);
        for (j = 1; j <= nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_TRACE_WATERMAN_SMITH_BEYER_GLOBAL_ALIGN(self->substitution_matrix[kA][kB]);
            gapM = PyMem_Malloc((i+1)*sizeof(int));
            if (!gapM) goto exit;
            gaps[i][j].MIx = gapM;
            gapXY = PyMem_Malloc((i+1)*sizeof(int));
            if (!gapXY) goto exit;
            gaps[i][j].IyIx = gapXY;
            nm = 0;
            ng = 0;
            score = -DBL_MAX;
            for (gap = 1; gap <= i; gap++) {
                ok = _call_query_gap_function(self, j, gap, &gapscore);
                if (!ok) goto exit;
                SELECT_TRACE_WATERMAN_SMITH_BEYER_GAP(M_scores[i-gap][j],
                                                      Iy_scores[i-gap][j]);
            }
            gapM = PyMem_Realloc(gapM, (nm+1)*sizeof(int));
            if (!gapM) goto exit;
            gaps[i][j].MIx = gapM;
            gapM[nm] = 0;
            gapXY = PyMem_Realloc(gapXY, (ng+1)*sizeof(int));
            if (!gapXY) goto exit;
            gapXY[ng] = 0;
            gaps[i][j].IyIx = gapXY;
            Ix_scores[i][j] = score;
            gapM = PyMem_Malloc((j+1)*sizeof(int));
            if (!gapM) goto exit;
            gaps[i][j].MIy = gapM;
            gapXY = PyMem_Malloc((j+1)*sizeof(int));
            if (!gapXY) goto exit;
            gaps[i][j].IxIy = gapXY;
            nm = 0;
            ng = 0;
            score = -DBL_MAX;
            for (gap = 1; gap <= j; gap++) {
                ok = _call_target_gap_function(self, i, gap, &gapscore);
                if (!ok) goto exit;
                SELECT_TRACE_WATERMAN_SMITH_BEYER_GAP(M_scores[i][j-gap],
                                                      Ix_scores[i][j-gap]);
            }
            Iy_scores[i][j] = score;
            gapM = PyMem_Realloc(gapM, (nm+1)*sizeof(int));
            if (!gapM) goto exit;
            gaps[i][j].MIy = gapM;
            gapM[nm] = 0;
            gapXY = PyMem_Realloc(gapXY, (ng+1)*sizeof(int));
            if (!gapXY) goto exit;
            gaps[i][j].IxIy = gapXY;
            gapXY[ng] = 0;
        }
    }

    /* traceback */
    SELECT_SCORE_GLOBAL(M_scores[nA][nB], Ix_scores[nA][nB], Iy_scores[nA][nB]);
    M[nA][nB].path = 0;

    if (M_scores[nA][nB] < score - epsilon) M[nA][nB].trace = 0;
    if (Ix_scores[nA][nB] < score - epsilon) {
        gapM = PyMem_Realloc(gaps[nA][nB].MIx, sizeof(int));
        if (!gapM) goto exit;
        gapM[0] = 0;
        gaps[nA][nB].MIx = gapM;
        gapXY = PyMem_Realloc(gaps[nA][nB].IyIx, sizeof(int));
        if (!gapXY) goto exit;
        gapXY[0] = 0;
        gaps[nA][nB].IyIx = gapXY;
    }
    if (Iy_scores[nA][nB] < score - epsilon) {
        gapM = PyMem_Realloc(gaps[nA][nB].MIy, sizeof(int));
        if (!gapM) goto exit;
        gapM[0] = 0;
        gaps[nA][nB].MIy = gapM;
        gapXY = PyMem_Realloc(gaps[nA][nB].IxIy, sizeof(int));
        if (!gapXY) goto exit;
        gapXY[0] = 0;
        gaps[nA][nB].IxIy = gapXY;
    }
    for (i = 0; i <= nA; i++) {
        PyMem_Free(M_scores[i]);
        PyMem_Free(Ix_scores[i]);
        PyMem_Free(Iy_scores[i]);
    }
    PyMem_Free(M_scores);
    PyMem_Free(Ix_scores);
    PyMem_Free(Iy_scores);
    return Py_BuildValue("fN", score, paths);

exit:
    if (ok) /* otherwise, an exception was already set */
        PyErr_SetNone(PyExc_MemoryError);
    Py_DECREF(paths);
    if (M_scores) {
        /* If M is NULL, then Ix is also NULL. */
        if (Ix_scores) {
            /* If Ix is NULL, then Iy is also NULL. */
            if (Iy_scores) {
                /* If Iy is NULL, then M[i], Ix[i], and Iy[i] are also NULL. */
                for (i = 0; i <= nA; i++) {
                    if (!M_scores[i]) break;
                    PyMem_Free(M_scores[i]);
                    if (!Ix_scores[i]) break;
                    PyMem_Free(Ix_scores[i]);
                    if (!Iy_scores[i]) break;
                    PyMem_Free(Iy_scores[i]);
                }
                PyMem_Free(Iy_scores);
            }
            PyMem_Free(Ix_scores);
        }
        PyMem_Free(M_scores);
    }
    return NULL;
}

static PyObject*
Aligner_waterman_smith_beyer_local_score(Aligner* self,
                                         const char* sA, Py_ssize_t nA,
                                         const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int gap;
    int kA;
    int kB;
    double** M = NULL;
    double** Ix = NULL;
    double** Iy = NULL;
    double score = 0.0;
    double gapscore = 0.0;
    double temp;
    int ok = 1;

    double maximum = 0.0;
    PyObject* result = NULL;

    /* Waterman-Smith-Beyer algorithm */
    M = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!M) goto exit;
    Ix = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!Ix) goto exit;
    Iy = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!Iy) goto exit;
    for (i = 0; i <= nA; i++) {
        M[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!M[i]) goto exit;
        Ix[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!Ix[i]) goto exit;
        Iy[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!Iy[i]) goto exit;
    }

    /* The top row of the score matrix is a special case,
     *  as there are no previously aligned characters.
     */
    M[0][0] = 0;
    Ix[0][0] = -DBL_MAX;
    Iy[0][0] = -DBL_MAX;
    for (i = 1; i <= nA; i++) {
        M[i][0] = -DBL_MAX;
        Ix[i][0] = 0;
        Iy[i][0] = -DBL_MAX;
    }
    for (j = 1; j <= nB; j++) {
        M[0][j] = -DBL_MAX;
        Ix[0][j] = -DBL_MAX;
        Iy[0][j] = 0;
    }
    for (i = 1; i <= nA; i++) {
        kA = CHARINDEX(sA[i-1]);
        for (j = 1; j <= nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            SELECT_SCORE_GOTOH_LOCAL_ALIGN(M[i-1][j-1],
                                           Ix[i-1][j-1],
                                           Iy[i-1][j-1],
                                           self->substitution_matrix[kA][kB]);
            M[i][j] = score;
            if (i == nA || j == nB) {
                Ix[i][j] = 0;
                Iy[i][j] = 0;
                continue;
            }
            score = 0.0;
            for (gap = 1; gap <= i; gap++) {
                ok = _call_query_gap_function(self, j, gap, &gapscore);
                SELECT_SCORE_WATERMAN_SMITH_BEYER(M[i-gap][j], Iy[i-gap][j]);
                if (!ok) goto exit;
            }
            if (score > maximum) maximum = score;
            Ix[i][j] = score;
            score = 0.0;
            for (gap = 1; gap <= j; gap++) {
                ok = _call_target_gap_function(self, i, gap, &gapscore);
                if (!ok) goto exit;
                SELECT_SCORE_WATERMAN_SMITH_BEYER(M[i][j-gap], Ix[i][j-gap]);
            }
            if (score > maximum) maximum = score;
            Iy[i][j] = score;
        }
    }
    SELECT_SCORE_GLOBAL(M[nA][nB], Ix[nA][nB], Iy[nA][nB]);
    if (score > maximum) maximum = score;

    result = PyFloat_FromDouble(maximum);
exit:
    if (M) {
        /* If M is NULL, then Ix is also NULL. */
        if (Ix) {
            /* If Ix is NULL, then Iy is also NULL. */
            if (Iy) {
                /* If Iy is NULL, then M[i], Ix[i], and Iy[i] are
                 * also NULL. */
                for (i = 0; i <= nA; i++) {
                    if (!M[i]) break;
                    PyMem_Free(M[i]);
                    if (!Ix[i]) break;
                    PyMem_Free(Ix[i]);
                    if (!Iy[i]) break;
                    PyMem_Free(Iy[i]);
                }
                PyMem_Free(Iy);
            }
            PyMem_Free(Ix);
        }
        PyMem_Free(M);
    }
    if (!ok) return NULL;
    if (!result) return PyErr_NoMemory();
    return result;
}

static PyObject*
Aligner_waterman_smith_beyer_local_align(Aligner* self,
                                         const char* sA, Py_ssize_t nA,
                                         const char* sB, Py_ssize_t nB)
{
    char c;
    int i;
    int j;
    int im = nA;
    int jm = nB;
    int gap;
    int kA;
    int kB;
    const double epsilon = self->epsilon;
    Trace** M = NULL;
    TraceGapsWatermanSmithBeyer** gaps;
    double** M_scores;
    double** Ix_scores = NULL;
    double** Iy_scores = NULL;
    double score;
    double gapscore;
    double temp;
    int trace;
    int* gapM;
    int* gapXY;
    int nm;
    int ng;
    int ok = 1;
    double maximum = 0;

    PathGenerator* paths = NULL;

    /* Waterman-Smith-Beyer algorithm */
    paths = PathGenerator_create_WSB(nA, nB, Local);
    if (!paths) return NULL;
    M = paths->M;
    gaps = paths->gaps.waterman_smith_beyer;

    M_scores = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!M_scores) goto exit;
    Ix_scores = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!Ix_scores) goto exit;
    Iy_scores = PyMem_Malloc((nA+1)*sizeof(double*));
    if (!Iy_scores) goto exit;
    for (i = 0; i <= nA; i++) {
        M_scores[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!M_scores[i]) goto exit;
        M_scores[i][0] = 0;
        Ix_scores[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!Ix_scores[i]) goto exit;
        Ix_scores[i][0] = -DBL_MAX;
        Iy_scores[i] = PyMem_Malloc((nB+1)*sizeof(double));
        if (!Iy_scores[i]) goto exit;
        Iy_scores[i][0] = -DBL_MAX;
    }
    for (i = 1; i <= nB; i++) {
        M_scores[0][i] = 0;
        Ix_scores[0][i] = -DBL_MAX;
        Iy_scores[0][i] = -DBL_MAX;
    }
    for (i = 1; i <= nA; i++) {
        kA = CHARINDEX(sA[i-1]);
        for (j = 1; j <= nB; j++) {
            kB = CHARINDEX(sB[j-1]);
            nm = 0;
            ng = 0;
            SELECT_TRACE_WATERMAN_SMITH_BEYER_ALIGN(
                                           M_scores[i-1][j-1],
                                           Ix_scores[i-1][j-1],
                                           Iy_scores[i-1][j-1],
                                           self->substitution_matrix[kA][kB]);
            M[i][j].path = 0;
            if (i == nA || j == nB) {
                Ix_scores[i][j] = score;
                gaps[i][j].MIx = NULL;
                gaps[i][j].IyIx = NULL;
                gaps[i][j].MIy = NULL;
                gaps[i][j].IxIy = NULL;
                Iy_scores[i][j] = score;
                continue;
            }
            gapM = PyMem_Malloc((i+1)*sizeof(int));
            if (!gapM) goto exit;
            gaps[i][j].MIx = gapM;
            gapXY = PyMem_Malloc((i+1)*sizeof(int));
            if (!gapXY) goto exit;
            gaps[i][j].IyIx = gapXY;
            score = -DBL_MAX;
            for (gap = 1; gap <= i; gap++) {
                ok = _call_query_gap_function(self, j, gap, &gapscore);
                if (!ok) goto exit;
                SELECT_TRACE_WATERMAN_SMITH_BEYER_GAP(M_scores[i-gap][j],
                                                      Iy_scores[i-gap][j]);
            }
            if (score < epsilon) {
                score = -DBL_MAX;
                nm = 0;
                ng = 0;
            }
            else if (score > maximum) maximum = score;
            gapM[nm] = 0;
            gapXY[ng] = 0;
            Ix_scores[i][j] = score;
            M[i][j].path = 0;
            gapM = PyMem_Realloc(gapM, (nm+1)*sizeof(int));
            if (!gapM) goto exit;
            gaps[i][j].MIx = gapM;
            gapM[nm] = 0;
            gapXY = PyMem_Realloc(gapXY, (ng+1)*sizeof(int));
            if (!gapXY) goto exit;
            gaps[i][j].IyIx = gapXY;
            gapXY[ng] = 0;
            gapM = PyMem_Malloc((j+1)*sizeof(int));
            if (!gapM) goto exit;
            gaps[i][j].MIy = gapM;
            gapXY = PyMem_Malloc((j+1)*sizeof(int));
            if (!gapXY) goto exit;
            gaps[i][j].IxIy = gapXY;
            nm = 0;
            ng = 0;
            score = -DBL_MAX;
            gapM[0] = 0;
            for (gap = 1; gap <= j; gap++) {
                ok = _call_target_gap_function(self, i, gap, &gapscore);
                if (!ok) goto exit;
                SELECT_TRACE_WATERMAN_SMITH_BEYER_GAP(M_scores[i][j-gap],
                                                      Ix_scores[i][j-gap]);
            }
            if (score < epsilon) {
                score = -DBL_MAX;
                nm = 0;
                ng = 0;
            }
            else if (score > maximum) maximum = score;
            gapM = PyMem_Realloc(gapM, (nm+1)*sizeof(int));
            if (!gapM) goto exit;
            gaps[i][j].MIy = gapM;
            gapXY = PyMem_Realloc(gapXY, (ng+1)*sizeof(int));
            if (!gapXY) goto exit;
            gaps[i][j].IxIy = gapXY;
            gapM[nm] = 0;
            gapXY[ng] = 0;
            Iy_scores[i][j] = score;
            M[i][j].path = 0;
        }
    }
    for (i = 0; i <= nA; i++) PyMem_Free(M_scores[i]);
    PyMem_Free(M_scores);
    for (i = 0; i <= nA; i++) PyMem_Free(Ix_scores[i]);
    PyMem_Free(Ix_scores);
    for (i = 0; i <= nA; i++) PyMem_Free(Iy_scores[i]);
    PyMem_Free(Iy_scores);

    /* As we don't allow zero-score extensions to alignments,
     * we need to remove all traces towards an ENDPOINT.
     * In addition, some points then won't have any path to a STARTPOINT.
     * Here, use path as a temporary variable to indicate if the point
     * is reachable from a STARTPOINT. If it is unreachable, remove all
     * traces from it, and don't allow it to be an ENDPOINT. It may still
     * be a valid STARTPOINT. */
    for (j = 0; j <= nB; j++) M[0][j].path = M_MATRIX;
    for (i = 1; i <= nA; i++) {
        M[i][0].path = M_MATRIX;
        for (j = 1; j <= nB; j++) {
            /* Remove traces to unreachable points. */
            trace = M[i][j].trace;
            if (!(M[i-1][j-1].path & M_MATRIX)) trace &= ~M_MATRIX;
            if (!(M[i-1][j-1].path & Ix_MATRIX)) trace &= ~Ix_MATRIX;
            if (!(M[i-1][j-1].path & Iy_MATRIX)) trace &= ~Iy_MATRIX;
            if (trace & (STARTPOINT | M_MATRIX | Ix_MATRIX | Iy_MATRIX)) {
                /* The point is reachable. */
                if (trace & ENDPOINT) M[i][j].path = 0; /* no extensions after ENDPOINT */
                else M[i][j].path |= M_MATRIX;
            }
            else {
                /* The point is not reachable. Then it is not a STARTPOINT,
                 * all traces from it can be removed, and it cannot act as
                 * an ENDPOINT. */
                M[i][j].path &= ~M_MATRIX;
                trace = 0;
            }
            M[i][j].trace = trace;
            if (i == nA || j == nB) continue;
            gapM = gaps[i][j].MIx;
            gapXY = gaps[i][j].IyIx;
            nm = 0;
            ng = 0;
            for (im = 0; (gap = gapM[im]); im++)
                if (M[i-gap][j].path & M_MATRIX) gapM[nm++] = gap;
            gapM = PyMem_Realloc(gapM, (nm+1)*sizeof(int));
            if (!gapM) goto exit;
            gapM[nm] = 0;
            gaps[i][j].MIx = gapM;
            for (im = 0; (gap = gapXY[im]); im++)
                if (M[i-gap][j].path & Iy_MATRIX) gapXY[ng++] = gap;
            gapXY = PyMem_Realloc(gapXY, (ng+1)*sizeof(int));
            if (!gapXY) goto exit;
            gapXY[ng] = 0;
            gaps[i][j].IyIx = gapXY;
            if (nm==0 && ng==0) M[i][j].path &= ~Ix_MATRIX; /* not reachable */
            else M[i][j].path |= Ix_MATRIX; /* reachable */
            gapM = gaps[i][j].MIy;
            gapXY = gaps[i][j].IxIy;
            nm = 0;
            ng = 0;
            for (im = 0; (gap = gapM[im]); im++)
                if (M[i][j-gap].path & M_MATRIX) gapM[nm++] = gap;
            gapM = PyMem_Realloc(gapM, (nm+1)*sizeof(int));
            if (!gapM) goto exit;
            gapM[nm] = 0;
            gaps[i][j].MIy = gapM;
            for (im = 0; (gap = gapXY[im]); im++)
                if (M[i][j-gap].path & Ix_MATRIX) gapXY[ng++] = gap;
            gapXY = PyMem_Realloc(gapXY, (ng+1)*sizeof(int));
            if (!gapXY) goto exit;
            gapXY[ng] = 0;
            gaps[i][j].IxIy = gapXY;
            if (nm==0 && ng==0) M[i][j].path &= ~Iy_MATRIX; /* not reachable */
            else M[i][j].path |= Iy_MATRIX; /* reachable */
        }
    }

    /* traceback */
    if (maximum == 0) M[0][0].path = DONE;
    else M[0][0].path = 0;

    return Py_BuildValue("fN", maximum, paths);

exit:
    if (ok) /* otherwise, an exception was already set */
        PyErr_SetNone(PyExc_MemoryError);
    Py_DECREF(paths);
    if (M_scores) {
        /* If M is NULL, then Ix is also NULL. */
        if (Ix_scores) {
            /* If Ix is NULL, then Iy is also NULL. */
            if (Iy_scores) {
                /* If Iy is NULL, then M[i], Ix[i], and Iy[i] are also NULL. */
                for (i = 0; i <= nA; i++) {
                    if (!M_scores[i]) break;
                    PyMem_Free(M_scores[i]);
                    if (!Ix_scores[i]) break;
                    PyMem_Free(Ix_scores[i]);
                    if (!Iy_scores[i]) break;
                    PyMem_Free(Iy_scores[i]);
                }
                PyMem_Free(Iy_scores);
            }
            PyMem_Free(Ix_scores);
        }
        PyMem_Free(M_scores);
    }
    return NULL;
}
 
static const char Aligner_score__doc__[] = "calculates the alignment score";

static PyObject*
Aligner_score(Aligner* self, PyObject* args, PyObject* keywords)
{
    const char* sA;
    const char* sB;
    Py_ssize_t nA;
    Py_ssize_t nB;
    const Mode mode = self->mode;
    const Algorithm algorithm = _get_algorithm(self);

    static char *kwlist[] = {"sequenceA", "sequenceB", NULL};
    if(!PyArg_ParseTupleAndKeywords(args, keywords, "s#s#", kwlist,
                                    &sA, &nA, &sB, &nB))
        return NULL;

    switch (algorithm) {
        case NeedlemanWunschSmithWaterman:
            switch (mode) {
                case Global:
                    return Aligner_needlemanwunsch_score(self, sA, nA, sB, nB);
                case Local:
                    return Aligner_smithwaterman_score(self, sA, nA, sB, nB);
            }
        case Gotoh:
            switch (mode) {
                case Global:
                    return Aligner_gotoh_global_score(self, sA, nA, sB, nB);
                case Local:
                    return Aligner_gotoh_local_score(self, sA, nA, sB, nB);
            }
        case WatermanSmithBeyer:
            switch (mode) {
                case Global:
                    return Aligner_waterman_smith_beyer_global_score(self, sA, nA, sB, nB);
                case Local:
                    return Aligner_waterman_smith_beyer_local_score(self, sA, nA, sB, nB);
            }
        case Unknown:
        default:
            PyErr_SetString(PyExc_RuntimeError, "unknown algorithm");
            return NULL;
    }
}

static const char Aligner_align__doc__[] = "align two sequences";

static PyObject*
Aligner_align(Aligner* self, PyObject* args, PyObject* keywords)
{
    const char* sA;
    const char* sB;
    Py_ssize_t nA;
    Py_ssize_t nB;
    const Mode mode = self->mode;
    const Algorithm algorithm = _get_algorithm(self);

    static char *kwlist[] = {"sequenceA", "sequenceB", NULL};
    if(!PyArg_ParseTupleAndKeywords(args, keywords, "s#s#", kwlist,
                                    &sA, &nA, &sB, &nB))
        return NULL;

    switch (algorithm) {
        case NeedlemanWunschSmithWaterman:
            switch (mode) {
                case Global:
                    return Aligner_needlemanwunsch_align(self, sA, nA, sB, nB);
                case Local:
                    return Aligner_smithwaterman_align(self, sA, nA, sB, nB);
            }
        case Gotoh:
            switch (mode) {
                case Global:
                    return Aligner_gotoh_global_align(self, sA, nA, sB, nB);
                case Local:
                    return Aligner_gotoh_local_align(self, sA, nA, sB, nB);
            }
        case WatermanSmithBeyer:
            switch (mode) {
                case Global:
                    return Aligner_waterman_smith_beyer_global_align(self, sA, nA, sB, nB);
                case Local:
                    return Aligner_waterman_smith_beyer_local_align(self, sA, nA, sB, nB);
            }
        case Unknown:
        default:
            PyErr_SetString(PyExc_RuntimeError, "unknown algorithm");
            return NULL;
    }
}

static char Aligner_doc[] =
"Aligner.\n";

static PyMethodDef Aligner_methods[] = {
    {"score",
     (PyCFunction)Aligner_score,
     METH_VARARGS | METH_KEYWORDS,
     Aligner_score__doc__
    },
    {"align",
     (PyCFunction)Aligner_align,
     METH_VARARGS | METH_KEYWORDS,
     Aligner_align__doc__
    },
    {NULL}  /* Sentinel */
};

static PyTypeObject AlignerType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_algorithms.PairwiseAligner", /* tp_name */
    sizeof(Aligner),               /* tp_basicsize */
    0,                             /* tp_itemsize */
    (destructor)Aligner_dealloc,   /* tp_dealloc */
    0,                             /* tp_print */
    0,                             /* tp_getattr */
    0,                             /* tp_setattr */
    0,                             /* tp_compare */
    (reprfunc)Aligner_repr,        /* tp_repr */
    0,                             /* tp_as_number */
    0,                             /* tp_as_sequence */
    0,                             /* tp_as_mapping */
    0,                             /* tp_hash */
    0,                             /* tp_call */
    (reprfunc)Aligner_str,         /* tp_str */
    0,                             /* tp_getattro */
    0,                             /* tp_setattro */
    0,                             /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,        /*tp_flags*/
    Aligner_doc,                   /* tp_doc */
    0,                             /* tp_traverse */
    0,                             /* tp_clear */
    0,                             /* tp_richcompare */
    0,                             /* tp_weaklistoffset */
    0,                             /* tp_iter */
    0,                             /* tp_iternext */
    Aligner_methods,               /* tp_methods */
    0,                             /* tp_members */
    Aligner_getset,                /* tp_getset */
    0,                             /* tp_base */
    0,                             /* tp_dict */
    0,                             /* tp_descr_get */
    0,                             /* tp_descr_set */
    0,                             /* tp_dictoffset */
    (initproc)Aligner_init,        /* tp_init */
};

/* Module definition */

static PyMethodDef _aligners_methods[] = {
    {NULL, NULL, 0, NULL}
};

static char _aligners__doc__[] =
"C extension module implementing pairwise alignment algorithms";

#if PY_MAJOR_VERSION >= 3

static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT,
        "_aligners",
        _aligners__doc__,
        -1,
        _aligners_methods,
        NULL,
        NULL,
        NULL,
        NULL
};

PyObject *
PyInit__aligners(void)

#else

void
init_aligners(void)
#endif

{
  PyObject* module;

  AlignerType.tp_new = PyType_GenericNew;

  if (PyType_Ready(&AlignerType) < 0
   || PyType_Ready(&PathGenerator_Type) < 0)
#if PY_MAJOR_VERSION >= 3
      return NULL;
#else
      return;
#endif

#if PY_MAJOR_VERSION >= 3
    module = PyModule_Create(&moduledef);
#else
    module = Py_InitModule3("_aligners", _aligners_methods, _aligners__doc__);
#endif

  Py_INCREF(&AlignerType);
  PyModule_AddObject(module, "PairwiseAligner", (PyObject*) &AlignerType);

#if PY_MAJOR_VERSION >= 3
  return module;
#endif
}
