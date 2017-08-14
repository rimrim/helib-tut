/* Copyright (C) 2012,2013 IBM Corp.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
/* DoubleCRT.cpp - This class holds an integer polynomial in double-CRT form
 *
 * Double-CRT form is a matrix of L rows and phi(m) columns. The i'th row
 * contains the FFT of the element wrt the ith prime, i.e. the evaluations of
 * the polynomial at the primitive mth roots of unity mod the ith prime. The
 * polynomial thus represented is defined modulo the product of all the primes
 * in use. The list of primes is defined by the data member modChain, which is
 * a vector of Cmodulus objects. 
 */
#include "DoubleCRT.h"
#include "timing.h"


#if (ALT_CRT)
#warning "Polynomial Arithmetic Implementation in AltCRT.cpp"
#include "AltCRT.cpp"
#else
#warning "Polynomial Arithmetic Implementation in DoubleCRT.cpp"

#include <NTL/ZZVec.h>
#include <NTL/BasicThreadPool.h>




// A threaded implementation of DoubleCRT operations

#ifdef FHE_DCRT_THREADS

static
long MakeIndexVector(const IndexSet& s, Vec<long>& v)
{
  long sz = s.card();
  v.SetLength(sz);
  for (long i = s.first(), j = 0; i <= s.last(); i = s.next(i), j++)
    v[j] = i;
  return sz;
}

// representing an integer polynomial as DoubleCRT. If the number of moduli
// to use is not specified, the resulting object uses all the moduli in
// the context. If the coefficients of poly are larger than the product of
// the used moduli, they are effectively reduced modulo that product


void DoubleCRT::FFT(const ZZX& poly, const IndexSet& s)
{
  FHE_TIMER_START;

  if (empty(s)) return;

  static thread_local Vec<long> tls_ivec;
  Vec<long>& ivec = tls_ivec;

  long icard = MakeIndexVector(s, ivec);
  NTL_EXEC_RANGE(icard, first, last)
      for (long j = first; j < last; j++) {
        long i = ivec[j];
        context.ithModulus(i).FFT(map[i], poly); 
      }
  NTL_EXEC_RANGE_END
}

// A non-threaded implementation of DoubleCRT operations
#else

void DoubleCRT::FFT(const ZZX& poly, const IndexSet& s)
{
  FHE_TIMER_START;


  if (empty(s)) return;
  for (long i = s.first(); i <= s.last(); i = s.next(i))
    context.ithModulus(i).FFT(map[i], poly);
}

#endif


// a "sanity check" function, verifies consistency of matrix with current
// moduli chain an error is raised if they are not consistent
void DoubleCRT::verify()
{
  assert(map.getIndexSet() <= (context.specialPrimes | context.ctxtPrimes));
  const IndexSet& s = map.getIndexSet();

  long phim = context.zMStar.getPhiM();

  // check that the content of i'th row is in [0,pi) for all i
  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    vec_long& row = map[i];

    if (row.length() != phim) 
      Error("DoubleCRT object has bad row length");

    long pi = context.ithPrime(i); // the i'th modulus
    for (long j=0; j<phim; j++)
      if (row[j]<0 || row[j]>= pi) 
	Error("DoubleCRT object has inconsistent data");
  }
}

// Arithmetic operations. Only the "destructive" versions are used,
// i.e., a += b is implemented but not a + b.

// Generic operation, Fnc is AddMod, SubMod, or MulMod (from NTL's ZZ module)
template<class Fun>
DoubleCRT& DoubleCRT::Op(const DoubleCRT &other, Fun fun,
			 bool matchIndexSets)
{
  if (isDryRun()) return *this;

  if (&context != &other.context)
    Error("DoubleCRT::Op: incompatible objects");

  // Match the index sets, if needed
  if (matchIndexSets && !(map.getIndexSet() >= other.map.getIndexSet()))
    addPrimes(other.map.getIndexSet() / map.getIndexSet()); // This is expensive

  // If you need to mod-up the other, do it on a temporary scratch copy
  DoubleCRT tmp(context, IndexSet()); 
  const IndexMap<vec_long>* other_map = &other.map;
  if (!(map.getIndexSet() <= other.map.getIndexSet())){ // Even more expensive
    tmp = other;
    tmp.addPrimes(map.getIndexSet() / other.map.getIndexSet());
    other_map = &tmp.map;
  }

  const IndexSet& s = map.getIndexSet();
  long phim = context.zMStar.getPhiM();

  // add/sub/mul the data, element by element, modulo the respective primes
  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    long pi = context.ithPrime(i);
    vec_long& row = map[i];
    const vec_long& other_row = (*other_map)[i];
    
    for (long j = 0; j < phim; j++)
      row[j] = fun.apply(row[j], other_row[j], pi);
  }
  return *this;
}

template
DoubleCRT& DoubleCRT::Op<DoubleCRT::MulFun>(const DoubleCRT &other, MulFun fun,
			 bool matchIndexSets);

template
DoubleCRT& DoubleCRT::Op<DoubleCRT::AddFun>(const DoubleCRT &other, AddFun fun,
			 bool matchIndexSets);

template
DoubleCRT& DoubleCRT::Op<DoubleCRT::SubFun>(const DoubleCRT &other, SubFun fun,
			 bool matchIndexSets);

template<class Fun>
DoubleCRT& DoubleCRT::Op(const ZZ &num, Fun fun)
{
  if (isDryRun()) return *this;

  const IndexSet& s = map.getIndexSet();
  long phim = context.zMStar.getPhiM();
  
  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    long pi = context.ithPrime(i);
    long n = rem(num, pi);  // n = num % pi
    vec_long& row = map[i];
    for (long j = 0; j < phim; j++)
      row[j] = fun.apply(row[j], n, pi);
  }
  return *this;
}

template
DoubleCRT& DoubleCRT::Op<DoubleCRT::MulFun>(const ZZ &num, MulFun fun);

template
DoubleCRT& DoubleCRT::Op<DoubleCRT::AddFun>(const ZZ &num, AddFun fun);

template
DoubleCRT& DoubleCRT::Op<DoubleCRT::SubFun>(const ZZ &num, SubFun fun);

DoubleCRT& DoubleCRT::Negate(const DoubleCRT& other)
{
  if (isDryRun()) return *this;

  if (&context != &other.context) 
    Error("DoubleCRT Negate: incompatible contexts");

  if (map.getIndexSet() != other.map.getIndexSet()) {
    map = other.map; // copy the data
  }
  const IndexSet& s = map.getIndexSet();
  long phim = context.zMStar.getPhiM();
  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    long pi = context.ithPrime(i);
    vec_long& row = map[i];
    const vec_long& other_row = other.map[i];
    for (long j = 0; j < phim; j++)
      row[j] = NegateMod(other_row[j], pi);
  }
  return *this;
}

template<class Fun>
DoubleCRT& DoubleCRT::Op(const ZZX &poly, Fun fun)
{
  if (isDryRun()) return *this;

  const IndexSet& s = map.getIndexSet();
  DoubleCRT other(poly, context, s); // other defined wrt same primes as *this

  return Op(other, fun);
}

template
DoubleCRT& DoubleCRT::Op<DoubleCRT::MulFun>(const ZZX &poly, MulFun fun);

template
DoubleCRT& DoubleCRT::Op<DoubleCRT::AddFun>(const ZZX &poly, AddFun fun);

template
DoubleCRT& DoubleCRT::Op<DoubleCRT::SubFun>(const ZZX &poly, SubFun fun);

// break *this into n digits,according to the primeSets in context.digits
void DoubleCRT::breakIntoDigits(vector<DoubleCRT>& digits, long n) const
{
  FHE_TIMER_START;
  IndexSet allPrimes = getIndexSet() | context.specialPrimes;
  assert(n <= (long)context.digits.size());

  digits.resize(n, DoubleCRT(context, IndexSet::emptySet()));
  if (isDryRun()) return;

  for (long i=0; i<(long)digits.size(); i++) {
    digits[i]=*this;
    IndexSet notInDigit = digits[i].getIndexSet()/context.digits[i];
    digits[i].removePrimes(notInDigit); // reduce modulo the digit primes
  }
  
  for (long i=0; i<(long)digits.size(); i++) {
    IndexSet notInDigit = allPrimes / digits[i].getIndexSet();
    digits[i].addPrimes(notInDigit); // add back all the primes

    // subtract this digits from all the others, then divide by pi
    ZZ pi = context.productOfPrimes(context.digits[i]);
    for (long j=i+1; j<(long)digits.size(); j++) {
      digits[j].Sub(digits[i], /*matchIndexSets=*/false);
      digits[j] /= pi;
    }
  }
#if 0
  dgts.resize(n, DoubleCRT(context, IndexSet::emptySet()));
  for (long i=0; i<n; i++) // copy only the primes for this digit
    dgts[i].partialCopy(*this, context.digits[i]);

  IndexSet allPrimes = getIndexSet() | context.specialPrimes;
  for (long i=0; i<n; i++) {
    IndexSet notInDigit = allPrimes / dgts[i].getIndexSet();
    dgts[i].addPrimes(notInDigit); // add back all the primes

    // subtract this digits from all the others, then divide by pi
    ZZ pi = context.productOfPrimes(context.digits[i]);
    for (long j=i+1; j<n; j++) {
      dgts[j].Sub(dgts[i], /*matchIndexSets=*/false);
      dgts[j] /= pi;
    }
  }
#endif
  FHE_TIMER_STOP;
}

// expand index set by s1.
// it is assumed that s1 is disjoint from the current index set.
void DoubleCRT::addPrimes(const IndexSet& s1)
{
  FHE_TIMER_START;

  if (empty(s1)) return; // nothing to do
  assert( disjoint(s1,map.getIndexSet()) ); // s1 is disjoint from *this

  ZZX poly;
  toPoly(poly); // recover in coefficient representation

  map.insert(s1);  // add new rows to the map
  if (isDryRun()) return;

  // fill in new rows
  FFT(poly, s1);
}

// Expand index set by s1, and multiply by \prod{q \in s1}. s1 is assumed to
// be disjoint from the current index set. Returns the logarithm of product.
double DoubleCRT::addPrimesAndScale(const IndexSet& s1)
{
  if (empty(s1)) return 0.0; // nothing to do
  assert(empty(s1 & map.getIndexSet())); // s1 is disjoint from *this

  // compute factor to scale existing rows
  ZZ factor = to_ZZ(1);
  double logFactor = 0.0;
  for (long i = s1.first(); i <= s1.last(); i = s1.next(i)) {
    long qi = context.ithPrime(i);
    factor *= qi;
    logFactor += log((double)qi);
  }

  // scale existing rows
  long phim = context.zMStar.getPhiM();
  const IndexSet& iSet = map.getIndexSet();
  for (long i = iSet.first(); i <= iSet.last(); i = iSet.next(i)) {
    long qi = context.ithPrime(i);
    long f = rem(factor, qi);     // f = factor % qi
    vec_long& row = map[i];
    // scale row by a factor of f modulo qi
    mulmod_precon_t bninv = PrepMulModPrecon(f, qi);
    for (long j=0; j<phim; j++) 
      row[j] = MulModPrecon(row[j], f, qi, bninv);
  }

  // insert new rows and fill them with zeros
  map.insert(s1);  // add new rows to the map
  for (long i = s1.first(); i <= s1.last(); i = s1.next(i)) {
    vec_long& row = map[i];
    for (long j=0; j<phim; j++) row[j] = 0;
  }

  return logFactor;
}

DoubleCRT::DoubleCRT(const ZZX& poly, const FHEcontext &_context, const IndexSet& s)
: context(_context), map(new DoubleCRTHelper(_context))
{
  FHE_TIMER_START;
  assert(s.last() < context.numPrimes());

  map.insert(s);
  if (isDryRun()) return;

  // convert the integer polynomial to FFT representation modulo the primes
  FFT(poly, s);
}

DoubleCRT::DoubleCRT(const ZZX& poly, const FHEcontext &_context)
: context(_context), map(new DoubleCRTHelper(_context))
{
  FHE_TIMER_START;
  IndexSet s = IndexSet(0, context.numPrimes()-1);
  // FIXME: maybe the default index set should be determined by context?

  map.insert(s);
  if (isDryRun()) return;

  // convert the integer polynomial to FFT representation modulo the primes
  FFT(poly, s);
}

DoubleCRT::DoubleCRT(const ZZX& poly)
: context(*activeContext), map(new DoubleCRTHelper(*activeContext))
{
  FHE_TIMER_START;
  IndexSet s = IndexSet(0, context.numPrimes()-1);
  // FIXME: maybe the default index set should be determined by context?

  map.insert(s);
  if (isDryRun()) return;

  // convert the integer polynomial to FFT representation modulo the primes
  FFT(poly, s);
}

DoubleCRT::DoubleCRT(const FHEcontext &_context, const IndexSet& s)
: context(_context), map(new DoubleCRTHelper(_context))
{
  assert(s.last() < context.numPrimes());

  map.insert(s);
  if (isDryRun()) return;

  long phim = context.zMStar.getPhiM();

  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    vec_long& row = map[i];
    for (long j = 0; j < phim; j++) row[j] = 0;
  }
}

DoubleCRT::DoubleCRT(const FHEcontext &_context)
: context(_context), map(new DoubleCRTHelper(_context))
{
  IndexSet s = IndexSet(0, context.numPrimes()-1);
  // FIXME: maybe the default index set should be determined by context?

  map.insert(s);
  if (isDryRun()) return;

  long phim = context.zMStar.getPhiM();

  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    vec_long& row = map[i];
    for (long j = 0; j < phim; j++) row[j] = 0;
  }
}

DoubleCRT& DoubleCRT::operator=(const DoubleCRT& other)
// optimized for the case of matching index sets
{
   if (this == &other) return *this;

   if (&context != &other.context) 
      Error("DoubleCRT assignment: incompatible contexts");

   if (map.getIndexSet() != other.map.getIndexSet()) {
      map = other.map; // copy the data
   }
   else {
      const IndexSet& s = map.getIndexSet();
      long phim = context.zMStar.getPhiM();
      for (long i = s.first(); i <= s.last(); i = s.next(i)) {
         vec_long& row = map[i];
         const vec_long& other_row = other.map[i];
         for (long j = 0; j < phim; j++)
            row[j] = other_row[j];
      }
   }
   return *this;
}

#if 0
// Copy only the primes in s \intersect other.getIndexSet()
void DoubleCRT::partialCopy(const DoubleCRT& other, const IndexSet& _s)
{
   if (&context != &other.context) 
      Error("DoubleCRT::partialCopy: incompatible contexts");

   // set the primes of *this to s \intersect other.getIndexSet()
   IndexSet s = _s;
   s.retain(other.getIndexSet());
   map.remove(getIndexSet() / s);
   map.insert(s / getIndexSet());

   long phim = context.zMStar.getPhiM();
   for (long i = s.first(); i <= s.last(); i = s.next(i)) {
     vec_long& row = map[i];
     const vec_long& other_row = other.map[i];
     for (long j = 0; j < phim; j++)
       row[j] = other_row[j];
   }
}
#endif

DoubleCRT& DoubleCRT::operator=(const ZZX&poly)
{
  if (isDryRun()) return *this;

  const IndexSet& s = map.getIndexSet();

  FFT(poly, s);

  return *this;
}

DoubleCRT& DoubleCRT::operator=(const ZZ& num)
{
  const IndexSet& s = map.getIndexSet();
  if (isDryRun()) return *this;

  long phim = context.zMStar.getPhiM();

  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    vec_long& row = map[i];
    long pi = context.ithPrime(i);
    long n = rem(num, pi);

    for (long j = 0; j < phim; j++) row[j] = n;
  }

  return *this;
}

// DIRT: this method affect the NTL zz_p::modulus
long DoubleCRT::getOneRow(zz_pX& row, long idx) const
{
  if (!map.getIndexSet().contains(idx)) // idx not in the primeset
    return 0;

  // convert from evaluation to standard coefficient representation
  context.ithModulus(idx).restoreModulus(); // recover NTL modulus for prime
  context.ithModulus(idx).iFFT(row, map[idx]);
  return context.ithPrime(idx);
}

// Get the row corresponding to the i'th moduli, in Vec<long> format.
// For conveience, returns the modulus that was used for this row.
// If idx is not in the current primesSet then do nothing and return 0;
long DoubleCRT::getOneRow(Vec<long>& row, long idx, bool positive) const
{
  zz_pBak bak; bak.save();   // backup NTL's current modulus

  zz_pX& tmp = Cmodulus::getScratch_zz_pX();
  long q = getOneRow(tmp,idx);
  if (q==0) return 0;        // no such index

  conv(row, tmp.rep);        // copy the row to Vec<long> format

  // By default, integers are in [0,q).
  // If we need the symmetric interval then make it so.
  if (!positive) {
    long phim = context.zMStar.getPhiM();
    for (long j = 0; j < phim; j++) if (row[j] > q/2) row[j] -= q;
  }
  return q;
}

#ifdef FHE_DCRT_THREADS
// experimental multi-threaded version



void DoubleCRT::toPoly(ZZX& poly, const IndexSet& s,
		       bool positive) const
{
FHE_TIMER_START;
  if (isDryRun()) return;

  IndexSet s1 = map.getIndexSet() & s;

  if (empty(s1)) {
    clear(poly);
    return;
  }


  static thread_local Vec<long> tls_ivec;
  static thread_local Vec<long> tls_pvec;
  static thread_local Vec< Vec<long> > tls_remtab;
  static thread_local Vec<zz_pX> tls_tmpvec;

  Vec<long>& ivec = tls_ivec;
  Vec<long>& pvec = tls_pvec;
  Vec< Vec<long> >& remtab = tls_remtab;
  Vec<zz_pX>& tmpvec = tls_tmpvec;

  long phim = context.zMStar.getPhiM();
  long icard = MakeIndexVector(s1, ivec);

  PartitionInfo pinfo(icard);
  long cnt = pinfo.NumIntervals();

  remtab.SetLength(phim);
  for (long h = 0; h < phim; h++) remtab[h].SetLength(icard);

  tmpvec.SetLength(cnt);
  for (long i = 0; i < cnt; i++) tmpvec[i].SetMaxLength(phim);

  { FHE_NTIMER_START(toPoly_FFT);
  
  NTL_EXEC_INDEX(cnt, index)
      long first, last;
      pinfo.interval(first, last, index);

      zz_pX& tmp = tmpvec[index];
  
      for (long j = first; j < last; j++) {
        long i = ivec[j];
        context.ithModulus(i).iFFT(tmp, map[i]); 
  
        long d = deg(tmp);
        for (long h = 0; h <= d; h++) remtab[h][j] = rep(tmp.rep[h]);
        for (long h = d+1; h < phim; h++) remtab[h][j] = 0;
      }
  NTL_EXEC_INDEX_END

  }

{ FHE_NTIMER_START(toPoly_CRT);

  PartitionInfo pinfo1(phim);
  long cnt1 = pinfo1.NumIntervals();

  static thread_local ZZ tls_prod;
  static thread_local ZZ tls_prod_half;
  static thread_local Vec<long> tls_qvec;
  static thread_local Vec<double> tls_qrecipvec;
  static thread_local Vec<long> tls_tvec;
  static thread_local Vec<mulmod_precon_t> tls_tqinvvec;

  static thread_local ZZVec tls_prod1vec;
  static thread_local ZZVec tls_resvec;

  ZZ& prod = tls_prod;
  ZZ& prod_half = tls_prod_half;
  ZZVec& prod1vec = tls_prod1vec;
  Vec<long>& qvec = tls_qvec;
  Vec<double>& qrecipvec = tls_qrecipvec;
  Vec<long>& tvec = tls_tvec;
  Vec<mulmod_precon_t>& tqinvvec = tls_tqinvvec;
  ZZVec& resvec = tls_resvec;

  qvec.SetLength(icard);
  qrecipvec.SetLength(icard);
  tvec.SetLength(icard);
  tqinvvec.SetLength(icard);

  prod = 1;
  for (long j = 0; j < icard; j++) {
    long i = ivec[j];
    long q = context.ithModulus(i).getQ();
    qvec[j] = q;
    qrecipvec[j] = 1/double(q);
    mul(prod, prod, q);
  }

  long sz = prod.size();

  if (prod1vec.length() != icard || prod1vec.BaseSize() != sz+1) {
    prod1vec.kill();
    prod1vec.SetSize(icard, sz+1);
  }

  for (long j = 0; j < icard; j++) {
    long q = qvec[j];
    div(prod1vec[j], prod, q);
    long t = rem(prod1vec[j], q);
    t = InvMod(t, q);
    tvec[j] = t;
    tqinvvec[j] = PrepMulModPrecon(t, q);
  }

  if (resvec.length() != phim || resvec.BaseSize() != sz+1) {
    resvec.kill();
    resvec.SetSize(phim, sz+1);
  }

  if (!positive) {
    // prod_half = (prod+1)/2
    add(prod_half, prod, 1);
    div(prod_half, prod_half, 2);
  }
  
  NTL_EXEC_INDEX(cnt1, index)
  NTL_IMPORT(icard)
      long first, last;
      pinfo1.interval(first, last, index);

      long *qvecp = qvec.elts();
      double *qrecipvecp = qrecipvec.elts();
      long *tvecp = tvec.elts();
      mulmod_precon_t *tqinvvecp = tqinvvec.elts();
      ZZ *prod1vecp = prod1vec.elts();

      ZZ tmp;
      tmp.SetSize(sz+4);
  
      for (long h = first; h < last; h++) {
        clear(tmp);
        double quotient = 0;
        long *remvec = remtab[h].elts();
        
        for (long j = 0; j < icard; j++) {
          long q = qvecp[j];
          long t = tvecp[j];
          mulmod_precon_t tqinv = tqinvvecp[j];
          long r = remvec[j];
          double qrecip = qrecipvecp[j];
          r = MulModPrecon(r, t, q, tqinv);
          MulAddTo(tmp, prod1vecp[j], r);
          quotient += r*qrecip;
        }
        
        MulSubFrom(tmp, prod, long(quotient));
        while (tmp < 0) add(tmp, tmp, prod);
        while (tmp >= prod) sub(tmp, tmp, prod);
        if (!positive && tmp >= prod_half) 
          tmp -= prod;
        resvec[h] = tmp;
      }
  NTL_EXEC_INDEX_END

  poly.SetLength(phim);
  for (long j = 0; j < phim; j++) poly[j] = resvec[j];
  poly.normalize();

  // NOTE: assigning to poly[j] within the parallel loop
  // leads to horrible performance, as there apparently is
  // a lot of contention within malloc.
}

}




#else



#if 1
// this version is faster than the original, which is based
// on NTL's built-in CRT routine
void DoubleCRT::toPoly(ZZX& poly, const IndexSet& s,
		       bool positive) const
{
FHE_TIMER_START;
  if (isDryRun()) return;

  IndexSet s1 = map.getIndexSet() & s;

  if (empty(s1)) {
    clear(poly);
    return;
  }

  zz_pBak bak; bak.save();

  long phim = context.zMStar.getPhiM();

  ZZ prod;
  prod = 1;
  for (long i = s1.first(); i <= s1.last(); i = s1.next(i))
    prod *= context.ithModulus(i).getQ();

  long sz = prod.size();

  ZZVec res;
  res.SetSize(phim, sz+1);

  Vec<double> quotients;
  quotients.SetLength(phim);
  for (long j = 0; j < phim; j++) quotients[j] = 0;

  ZZ prod1;

  for (long i = s1.first(); i <= s1.last(); i = s1.next(i)) {
    context.ithModulus(i).restoreModulus();
    zz_pX& tmp = Cmodulus::getScratch_zz_pX();
    context.ithModulus(i).iFFT(tmp, map[i]); 
    {
      FHE_NTIMER_START(toPoly_CRT);

      long q = zz_p::modulus();
      mulmod_t qinv = zz_p::ModulusInverse();
      div(prod1, prod, q);
      long r = rem(prod1, q);
      long rinv = InvMod(r, q);
      mulmod_precon_t rinvqinv = PrepMulModPrecon(rinv, q, qinv);
      long tlen = tmp.rep.length();
      const zz_p *tp = tmp.rep.elts();
      double qrecip = 1/double(q);

      for (long j = 0; j < tlen; j++) {
        long s2 = MulModPrecon(rep(tp[j]), rinv, q, rinvqinv);
        MulAddTo(res[j], prod1, s2);
        quotients[j] += s2*qrecip;
      }
    }
  }


  if (positive) {
    FHE_NTIMER_START(toPoly_finish);
    poly.rep.SetLength(phim);
    for (long j = 0; j < phim; j++) {
      if (res[j] >= prod) rem(res[j], res[j], prod);
      poly.rep[j] = res[j];
    }
    poly.normalize();
  }
  else {


#if 0
    FHE_NTIMER_START(toPoly_finish);
    div(prod1, prod, 2);
    poly.rep.SetLength(phim);
    for (long j = 0; j < phim; j++) {
      if (res[j] >= prod) rem(res[j], res[j], prod);
      if (res[j] > prod1) sub(res[j], res[j], prod);
      poly.rep[j] = res[j];
    }
    poly.normalize();
#elif 1
    FHE_NTIMER_START(toPoly_finish);
    div(prod1, prod, 2);
    poly.rep.SetLength(phim);
    ZZ t1;
    for (long j = 0; j < phim; j++) {
      t1 = res[j];
      MulSubFrom(t1, prod, long(quotients[j]));
      while (t1 < 0) add(t1, t1, prod);
      while (t1 >= prod) sub(t1, t1, prod);
      res[j] = t1;
      if (res[j] > prod1) sub(res[j], res[j], prod);
      poly.rep[j] = res[j];
    }
    poly.normalize();
#else
    FHE_NTIMER_START(toPoly_finish1);
    div(prod1, prod, 2);
    poly.rep.SetLength(phim);
    FHE_NTIMER_STOP(toPoly_finish1);

    ZZ t1;
    
    FHE_NTIMER_START(toPoly_finish2);
    for (long j = 0; j < phim; j++) {
#if 1
      t1 = res[j];
      MulSubFrom(t1, prod, long(quotients[j]));
      while (t1 < 0) add(t1, t1, prod);
      while (t1 >= prod) sub(t1, t1, prod);
      res[j] = t1;
#else
      if (res[j] >= prod) rem(res[j], res[j], prod);
#endif
    }
    FHE_NTIMER_STOP(toPoly_finish2);

    FHE_NTIMER_START(toPoly_finish3);
    for (long j = 0; j < phim; j++) {
      if (res[j] > prod1) sub(res[j], res[j], prod);
    }
    FHE_NTIMER_STOP(toPoly_finish3);

    FHE_NTIMER_START(toPoly_finish4);
    for (long j = 0; j < phim; j++) {
      poly.rep[j] = res[j];
    }
    poly.normalize();
    FHE_NTIMER_STOP(toPoly_finish4);

#endif




  }
}
#else
void DoubleCRT::toPoly(ZZX& poly, const IndexSet& s,
		       bool positive) const
{
FHE_TIMER_START;
  if (isDryRun()) return;

  IndexSet s1 = map.getIndexSet() & s;

  if (empty(s1)) {
    clear(poly);
    return;
  }

  clear(poly);
  ZZ prod;
  prod = 1;

  zz_pBak bak; bak.save();

  for (long i = s1.first(); i <= s1.last(); i = s1.next(i)) {
    context.ithModulus(i).restoreModulus();
    zz_pX& tmp = Cmodulus::getScratch_zz_pX();
    context.ithModulus(i).iFFT(tmp, map[i]); 
    {
       FHE_NTIMER_START(toPoly_CRT);
       CRT(poly, prod, tmp);  // NTL :-)
    }
  }

  if (positive) {
    long d = deg(poly);
    for (long j = 0; j <= d; j++) 
      if (poly.rep[j] < 0)
        poly.rep[j] += prod;   

    // no need to normalize poly here
  }
}



#endif




#endif















#if 0
{
FHE_TIMER_START;
  if (isDryRun()) return;

  IndexSet s1 = map.getIndexSet() & s;

  if (empty(s1)) {
    clear(poly);
    return;
  }

  ZZ p = to_ZZ(context.ithPrime(s1.first()));  // the first modulus

  // Get poly modulo the first prime in coefficent form
  long i = s1.first();
  const Cmodulus& mod = context.ithModulus(i);
  mod.iFFT(poly, map[i]);

  vec_ZZ& vp = poly.rep;

  // ensure that vp is of size phi(m) with entries in [-p/2,p/2]
  long phim = context.zMStar.getPhiM();
  long vpLength = vp.length();
  if (vpLength < phim) { // just in case of leading zeros in poly
    vp.SetLength(phim);
    for (long j = vpLength; j < phim; j++) vp[j]=0;
  }
  ZZ p_over_2 = p/2;
  for (long j = 0; j < phim; j++) if (vp[j] > p_over_2) vp[j] -= p;

  // do incremental integer CRT for other levels

  ZZX current;
  for (i = s1.next(i); i <= s1.last(); i = s1.next(i)) {
    long q = context.ithPrime(i);          // the next modulus
    context.ithModulus(i).iFFT(current, map[i]); // Poly mod q in coeff form

    // CRT the coefficient vectors of poly and current
    intVecCRT(vp, p, current.rep, q);    // defined in the module NumbTh
    p *= q;     // update the modulus
  }

  // The above yeilds polynomial with coefficients in [-p/2,p/2]
  // If we need positive, just add p to all the negative coefficients
  if (positive) 
    for (long j=0; j<poly.rep.length(); j++) {
      if (poly.rep[j] < 0) poly.rep[j] += p;
    }

  poly.normalize(); // need to call this after we work on the coeffs
FHE_TIMER_STOP;
}
#endif

void DoubleCRT::toPoly(ZZX& p, bool positive) const
{
  const IndexSet& s = map.getIndexSet();
  toPoly(p, s, positive);
}

// Division by constant
DoubleCRT& DoubleCRT::operator/=(const ZZ &num)
{
  if (isDryRun()) return *this;

  const IndexSet& s = map.getIndexSet();
  long phim = context.zMStar.getPhiM();
  
  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    long pi = context.ithPrime(i);
    long n = InvMod(rem(num, pi),pi);  // n = num^{-1} mod pi
    vec_long& row = map[i];
    mulmod_precon_t precon = PrepMulModPrecon(n, pi);
    for (long j = 0; j < phim; j++)
      row[j] = MulModPrecon(row[j], n, pi, precon);
  }
  return *this;
}

// Small-exponent polynomial exponentiation
void DoubleCRT::Exp(long e)
{
  if (isDryRun()) return;

  const IndexSet& s = map.getIndexSet();
  long phim = context.zMStar.getPhiM();
  
  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    long pi = context.ithPrime(i);
    vec_long& row = map[i];
    for (long j = 0; j < phim; j++)
      row[j] = PowerMod(row[j], e, pi);
  }
}

// Apply the automorphism F(X) --> F(X^k)  (with gcd(k,m)=1)
void DoubleCRT::automorph(long k)
{
  if (isDryRun()) return;

  const PAlgebra& zMStar = context.zMStar;
  if (!zMStar.inZmStar(k))
    Error("DoubleCRT::automorph: k not in Zm*");

  long m = zMStar.getM();
  vector<long> tmp(m);  // temporary array of size m
  mulmod_precon_t precon = PrepMulModPrecon(k, m);

  const IndexSet& s = map.getIndexSet();

  // go over the rows, permute them one at a time
  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    vec_long& row = map[i];

    for (long j=1; j<m; j++) { // 1st pass: copy to temporary array
      long idx = zMStar.indexInZmstar(j); // returns -1 if j \notin (Z/mZ)*
      if (idx>=0) tmp[j] = row[idx];
    }

    for (long j=1; j<m; j++) { // 2nd pass: copy back from temporary array
      long idx = zMStar.indexInZmstar(j); // returns -1 if j \notin (Z/mZ)*
      if (idx>=0) row[idx] = tmp[MulModPrecon(j,k,m,precon)];
                                           // new[j] = old[j*k mod m]
    }    
  }
}

// fills each row i with random integers mod pi
#if 1
void DoubleCRT::randomize(const ZZ* seed) 
{
  FHE_TIMER_START;

  if (isDryRun()) return;

  if (seed != NULL) SetSeed(*seed);

  const IndexSet& s = map.getIndexSet();
  long phim = context.zMStar.getPhiM();

  RandomStream& stream = GetCurrentRandomStream();
  const long bufsz = 2048;

  Vec<unsigned char> buf_storage;
  buf_storage.SetLength(bufsz);

  unsigned char *buf = buf_storage.elts();

  
  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    long pi = context.ithPrime(i);
    long k = NumBits(pi-1);
    long nb = (k+7)/8;
    unsigned long mask = (1UL << k) - 1UL;

    vec_long& row = map[i];
    long j = 0;
    
    for (;;) {
      stream.get(buf, bufsz);

      for (long pos = 0; pos <= bufsz-nb; pos += nb) {
        unsigned long utmp = 0;
        for (long cnt = nb-1;  cnt >= 0; cnt--)
          utmp = (utmp << 8) | buf[pos+cnt]; 
        utmp = (utmp & mask);
        
        long tmp = utmp;

        row[j] = tmp;
        j += (tmp < pi);
        if (j >= phim) break;
      }
      if (j >= phim) break;
    }
  }
}


#else
void DoubleCRT::randomize(const ZZ* seed) 
{
  FHE_TIMER_START;

  if (isDryRun()) return;

  if (seed != NULL) SetSeed(*seed);

  const IndexSet& s = map.getIndexSet();
  long phim = context.zMStar.getPhiM();

  ZZ prod;
  context.productOfPrimes(prod, s);

  Vec<ZZ> vec;
  vec.SetLength(phim);
  for (long j = 0; j < phim; j++)
    RandomBnd(vec[j], prod);
  
  zz_pBak bak; bak.save();

  // DIRT: this Vec<zz_p> is used for several moduli
  NTL_THREAD_LOCAL static Vec<zz_p> vvec_tls;
  Vec<zz_p>& vvec = vvec_tls;
  vvec.SetLength(phim);
  
  for (long i = s.first(); i <= s.last(); i = s.next(i)) {
    context.ithModulus(i).restoreModulus();
    conv(vvec, vec);

    long *row = map[i].elts();
    const zz_p *vvecp = vvec.elts();
    for (long j = 0; j < phim; j++)
      row[j] = rep(vvecp[j]);  
  }
}
#endif

void DoubleCRT::scaleDownToSet(const IndexSet& s, long ptxtSpace)
{
  assert(ptxtSpace >= 2);

  IndexSet diff = getIndexSet() / s;
  assert(diff!=s);          // cannot mod-down to the empty set
  if (empty(diff)) return;  // nothing to do

  if (isDryRun()) {
    removePrimes(diff);// remove the primes from consideration
    return;
  }

  ZZX delta;
  ZZ diffProd = context.productOfPrimes(diff); // mod-down by this factor
  toPoly(delta, diff); // convert to coeff-representation modulo diffProd

  long delta_len = delta.rep.length();
  if (ptxtSpace == 2) { // simpler handling for plaintext space mod 2
    for (long i = 0; i < delta_len; i++) {
      if (IsOdd(delta.rep[i])) { // add or subtract diffProd to make it even
	if (sign(delta.rep[i]) < 0) delta.rep[i] += diffProd;
	else                        delta.rep[i] -= diffProd;
      }
    }
  }
  // The general case of plaintext space modulo some p > 2, we need to
  // subtract from each coefficient delta[i] the ineteger
  //               diffProd * (delta[i] * diffProd^{-1} mod ptxtSpace).
  // This does not change delta modulo diffProd, but makes it divisible
  // by ptxtSpace.
  else {
    long p_over_2 = ptxtSpace/2;
    long prodInv = InvMod(rem(diffProd,ptxtSpace), ptxtSpace);
    mulmod_precon_t precon = PrepMulModPrecon(prodInv, ptxtSpace); // optimization
    for (long i = 0; i < delta_len; i++) {
      long delta_i_modP = rem(delta.rep[i],ptxtSpace);
      if (delta_i_modP != 0) { // if not already 0 mod ptxtSpace
	delta_i_modP = MulModPrecon(delta_i_modP, prodInv, ptxtSpace, precon);
	if (delta_i_modP > p_over_2) delta_i_modP -= ptxtSpace;
	delta.rep[i] -= diffProd * delta_i_modP;
      }
    }
  }
  delta.normalize(); // need to normalize after working directly on then coeffs

  removePrimes(diff);// remove the primes from consideration
  *this -= delta;    // convert delta to DoubleCRT, then subtract
  *this /= diffProd; // *this is divisible by diffProd, so this operation actually scales it down
}

ostream& operator<< (ostream &str, const DoubleCRT &d)
{
  const IndexSet& set = d.map.getIndexSet();

  // check that the content of i'th row is in [0,pi) for all i
  str << "[" << set << endl;
  for (long i = set.first(); i <= set.last(); i = set.next(i))
    str << " " << d.map[i] << "\n";
  str << "]";
  return str;
}

istream& operator>> (istream &str, DoubleCRT &d)
{
  //  cerr << "DoubleCRT[";
  // Advance str beyond first '[' 
  seekPastChar(str, '[');  // this function is defined in NumbTh.cpp

  IndexSet set;
  const FHEcontext& context = d.context;
  long phim = context.zMStar.getPhiM();

  str >> set; // read in the indexSet
  assert(set <= (context.specialPrimes | context.ctxtPrimes));
  d.map.clear();
  d.map.insert(set); // fix the index set for the data

  for (long i = set.first(); i <= set.last(); i = set.next(i)) {
    str >> d.map[i]; // read the actual data

    // verify that the data is valid
    assert (d.map[i].length() == phim);
    for (long j=0; j<phim; j++)
      assert(d.map[i][j]>=0 && d.map[i][j]<context.ithPrime(i));
  }

  // Advance str beyond closing ']'
  seekPastChar(str, ']');
  //  cerr << "]";
  return str;
}
#endif

