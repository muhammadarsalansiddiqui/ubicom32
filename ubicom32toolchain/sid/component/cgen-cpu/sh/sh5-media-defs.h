/* CPU family header for sh5 / sh5_media.

THIS FILE IS MACHINE GENERATED WITH CGEN.

Copyright (C) 2000-2005 Red Hat, Inc.

This file is part of the Red Hat simulators.


*/

#ifndef DEFS_SH5_MEDIA_H
#define DEFS_SH5_MEDIA_H

#include <stack>
#include "cgen-types.h"

// forward declaration

  
namespace sh5 {
struct sh5_cpu;
}

namespace sh5_media {

using namespace cgen;

  static const int max_delay = 0;
  static const int pipe_sz = 1; // max_delay + 1

  template <typename ELT> 
  struct write_stack 
  {
    int t;
    const int sz;
    ELT buf[WRITE_BUF_SZ];

    write_stack       ()             : t(-1), sz(WRITE_BUF_SZ) {}
    inline bool empty ()             { return (t == -1); }
    inline void clear ()             { t = -1; }
    inline void pop   ()             { if (t > -1) t--;}
    inline void push  (const ELT &e) { if (t+1 < sz) buf [++t] = e;}
    inline ELT &top   ()             { return buf [t>0 ? ( t<sz ? t : sz-1) : 0];}
  };

  // look ahead for latest write with index = idx, where time of write is
  // <= dist steps from base (present) in write_stack array st.
  // returning def if no scheduled write is found.

  template <typename STKS, typename VAL>
  inline VAL lookahead (int dist, int base, STKS &st, VAL def, int idx=0)
  {
    for (; dist > 0; --dist)
    {
      write_stack <VAL> &v = st [(base + dist) % pipe_sz];
      for (int i = v.t; i > 0; --i) 
	  if (v.buf [i].idx0 == idx) return v.buf [i];
    }
    return def;
  }



  template <typename MODE>
  struct write
  {
    USI pc;
    MODE val;
    USI idx0;
    write (PCADDR _pc, MODE _val, USI _idx0=0) : pc(_pc), val(_val), idx0(_idx0) {} 
    write() {}
  };


// write stacks used in parallel execution

  struct write_stacks
  {
  // types of stacks



  // unified writeback function (defined in sh5_media-write.cc)
  void writeback (int tick, sh5::sh5_cpu* current_cpu);
  // unified write-stack clearing function (defined in sh5_media-write.cc)
  void reset ();

  }; // end struct sh5_media::write_stacks 

} // end sh5_media namespace

#endif /* DEFS_SH5_MEDIA_H */