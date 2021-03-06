/* { dg-do run } */
/* { dg-options "-fdump-tree-alias" } */
/* { dg-skip-if "" { *-*-* } { "-O0" } { "" } } */

int i;
void __attribute__((noinline))
foo(long b, long q)
{
  int *y;
  int **a = &y, **x;
  int ***p;
  if (b)
    p = (int ***)q;
  else
    p = &a;
  x = *p;
  *x = &i;  /* *ANYTHING = &i has to make sure that y points to i.  */
  *y = 0;
}
extern void abort (void);
int main()
{
  i = 1;
  foo (0, 0);
  if (i != 0)
    abort ();
  return 0;
}

/* { dg-final { scan-tree-dump "y.._., name memory tag: NMT..., is dereferenced, points-to vars: { i }" "alias" } } */
/* { dg-final { cleanup-tree-dump "alias" } } */
