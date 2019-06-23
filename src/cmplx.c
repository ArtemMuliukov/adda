/* File: cmplx.c
 * $Date::                            $
 * Descr: functions for computing complex exponents, too large to be kept inline
 *
 * Copyright (C) 2019 ADDA contributors
 * This file is part of ADDA.
 *
 * ADDA is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * ADDA is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with ADDA. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "const.h" // keep this first
#include "cmplx.h" // corresponding header
// project headers
#include "types.h" // for doublecomplex
#include "io.h"    // for LogError
// system headers
#include <float.h>
#include <limits.h>   // for INT_MAX
#include <math.h>

#ifndef NO_IMEXP_TABLE
static doublecomplex ieTable[361]; // table for imExpTable

//======================================================================================================================

void imExpTableInit()
/* initialize table for imExp; it is slightly optimized (8 times) just for fun (run only once anyway)
 * further optimization is not done, since it will cause accuracy loss
 */
{
	int i;
	ieTable[0]=ieTable[360]=1;
	ieTable[45]=SQRT1_2*(1+I);
	for (i=1;i<45;i++) { // fill first quadrant using symmetries
		ieTable[i]=cexp(I*Deg2Rad(i));
		ieTable[90-i]=I*conj(ieTable[i]);
	}
	for (i=0;i<90;i++) { // fill other three quadrants
		ieTable[i+90]=I*ieTable[i];
		ieTable[i+180]=-ieTable[i];
		ieTable[i+270]=-I*ieTable[i];
	}
}

//======================================================================================================================

doublecomplex imExpTable(const double arg)
/* exponent of imaginary argument Exp(i*arg) accelerated with lookup table - the table must be initialized before use
 * the idea is based on previous SSE3 code that was adapted from CEPHES library
 */
{
	// we do not use static variables for thread-safety (although they do lead to a few % speedup)
	double xp;
	int ixp;
	double x;
	doublecomplex imexp;
	doublecomplex res;
	const double abs_arg = fabs(arg);
	bool neg = (arg<0);

	/* First, we scale the argument to degrees and bring it to [0,360) range
	 * This range-reduction is a standard operation, and there are two main options for it. One can use TWO_PI with
	 * larger than double precision (routines like __ieee754_rem_pio2), but they are rather slow. Alternatively, we use
	 * simple operations with only double precision, and lose precision for large arguments. However, this loss of
	 * precision is inherent in trying to compute cexp of a larger argument. We can compute cexp(I*arg) to a good
	 * accuracy, but changing the argument by 1 lowest bit (relative uncertainty eps) introduces relative change to the
	 * answer of arg*eps. So we 	 * choose the fast range reduction which precision loss is the same as the inherent
	 * one.
	 */
	xp=abs_arg/TWO_PI;
	if (xp>INT_MAX) { // slow case, but needed for robustness
		xp=modf(xp,&x);
	}
	else {
		ixp=(int)xp;
		xp=xp-ixp;
	}
	x=xp*FULL_ANGLE;
	// Second, it is divided into integer degrees (rounded) and residual x (|x|<=0.5)
	ixp=(int)(x+0.5); // fast round, here large numbers are not an issue
	x=x-ixp;          // residual
	xp=x*x;
	imexp=ieTable[ixp];
	/* Alternative way is to use two smaller tables (e.g. 18x20) to have less memory footprint at the expense of a few
	 * more arithmetic operations (for indexing and then multiplication of results). Does not make a lot of sense, but
	 * can be useful in case of small caches
	 */

	/* Third, Taylor series is used for residual. An=(-1)^floor(n/2)*(pi/180)^n/n!
	 * The following is constructed to provide double accuracy for argument (in degrees) less than 0.5; it should be
	 * changed if float or long double precision is required.
	 * Potentially it can be replaced by Chebyshev series, but the improvement can be negligible on such short series.
	 */
#define A1  1.7453292519943295769236907684886e-2
#define A2 -1.5230870989335429967337177468945e-4
#define A3 -8.8609615570129801598869213154725e-7
#define A4  3.8663238515629936539637763508129e-9
#define A5  1.3496016231632550105929914052817e-11
#define A6 -3.9258319857430948822261807485761e-14
	res=((A6+I*A5)*xp + (A4+I*A3))*xp + (A2+I*A1);
	res=1 + creal(res)*xp + I*cimag(res)*x;
	imexp*=res;
#undef A1
#undef A2
#undef A3
#undef A4
#undef A5
#undef A6
	if (neg) imexp=conj(imexp);
	/* The following can be used to test the accuracy of the fast implementations, and to estimate the relative time
	 * used by imExp in test matvec runs (since it adds invocation of cexp).
	 */
//	doublecomplex ref=cexp(I*arg);
//	double err=cabs(imexp-ref);
//	if (err>2*MAX(fabs(arg),1)*DBL_EPSILON) {
//		LogError(ALL_POS,"Insufficient accuracy of accelerated imExp: arg="GFORM_FULL"; err="GFORMDEF"\n"
//			"Computed:  "CFORM_FULL"\nReference: "CFORM_FULL,arg,err,REIM(imexp),REIM(ref));
//	}
	return imexp;
}

#endif // NO_IMEXP_TABLE

//======================================================================================================================

void imExp_arr(const doublecomplex arg,const int size,doublecomplex *c)
/* construct an array of exponent of imaginary argument c=Exp(i*k*arg), where k=0,1,...,size-1. arg can be complex.
 * Uses stable recurrence from Numerical Recipes. Optimization of the initial simultaneous calculation of sin and cos is
 * performed by compiler; It is assumed that size is at least 1
 */
{
	int k;
	double a,b;
	doublecomplex d,tmp;
	double re,im;

	re=creal(arg);
	im=cimag(arg);
	// handles real part, no special case for re=0
	c[0]=1;
	if (size>1) {
		// set a=2*sin^2(arg/2), b=sin(arg), d = 1 - exp(i*arg)
		a=sin(re/2);
		b=cos(re/2);
		b*=2*a;
		a*=2*a;
		d= a - I*b;
		// this a bit faster than in the main cycle
		c[1]=1-d;
		// main cycle
		for (k=2;k<size;k++) {
			/* potentially compiler may group terms to accelerate calculation but lose significant digits. We hope it
			 * doesn't happen, but it should not be a big problem anyway
			 */
			tmp=c[k-1]*d;
			c[k]=c[k-1]-tmp;
		}
	}
	// handles imaginary part
	if (im!=0) {
		a=exp(-fabs(im));
		if (im>0) for (k=1,b=a;k<size;k++) {
			c[k]*=b;
			b*=a;
		}
		else for (k=size-1,b=exp(-(size-1)*im);k>0;k--) {
			c[k]*=b;
			b*=a;
		}
	}
}
