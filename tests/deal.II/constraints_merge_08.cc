//----------------------------  constraints_merge_08.cc  ---------------------------
//    $Id$
//    Version: $Name$
//
//    Copyright (C) 1998, 1999, 2000, 2001, 2003, 2004, 2005, 2008, 2010, 2011 by the deal.II authors
//
//    This file is subject to QPL and may not be  distributed
//    without copyright and license information. Please refer
//    to the file deal.II/doc/license.html for the  text  and
//    further information on this license.
//
//----------------------------  constraints_merge_08.cc  ---------------------------


// merge and print a bunch of ConstrainMatrices. test the case that we have
// inhomogeneities in the constraints and the constraint matrix is constructed
// based on an IndexSet to transform large global indices into local ones

#include "../tests.h"
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/base/logstream.h>

#include <fstream>
#include <iomanip>


std::ofstream logfile("constraints_merge_08/output");


void merge_check ()
{
  deallog << "Checking ConstraintMatrix::merge with localized lines" << std::endl;

				// set local lines to a very large range that
				// surely triggers an error if the
				// implementation is wrong
  IndexSet local_lines (100000000);
  local_lines.add_range (99999890, 99999900);
  local_lines.add_range (99999990,100000000);
  local_lines.compress();

				// the test is the same as
				// constraints_merge_02, but we add very large
				// indices here
  const unsigned int index_0  = local_lines.nth_index_in_set(0);
  const unsigned int index_1  = local_lines.nth_index_in_set(1);
  const unsigned int index_3  = local_lines.nth_index_in_set(3);
  const unsigned int index_4  = local_lines.nth_index_in_set(4);
  const unsigned int index_10 = local_lines.nth_index_in_set(10);
  const unsigned int index_11 = local_lines.nth_index_in_set(11);
  const unsigned int index_12 = local_lines.nth_index_in_set(12);
  const unsigned int index_13 = local_lines.nth_index_in_set(13);

  deallog << "Number of local lines: "
	  << local_lines.n_elements() << std::endl;

				   // check twice, once with closed
				   // objects, once with open ones
  for (unsigned int run=0; run<2; ++run)
    {
      deallog << "Checking with " << (run == 0 ? "open" : "closed")
	      << " objects" << std::endl;

				       // check that the `merge' function
				       // works correctly
      ConstraintMatrix c1 (local_lines), c2 (local_lines);

				       // enter simple line
      c1.add_line          (index_0);
      c1.add_entry         (index_0, index_11, 1.);
      c1.set_inhomogeneity (index_0, 42);

				       // add more complex line
      c1.add_line          (index_1);
      c1.add_entry         (index_1, index_3,  0.5);
      c1.add_entry         (index_1, index_4,  0.5);
      c1.set_inhomogeneity (index_1, 100);

				       // fill second constraints
				       // object with one trivial line
				       // and one which further
				       // constrains one of the
				       // entries in the first object
      c2.add_line          (index_10);
      c2.add_entry         (index_10, index_11, 1.);
      c2.set_inhomogeneity (index_10, 142);

      c2.add_line          (index_3);
      c2.add_entry         (index_3, index_12, 0.25);
      c2.add_entry         (index_3, index_13, 0.75);
      c2.set_inhomogeneity (index_3, 242);

				       // in one of the two runs,
				       // close the objects
      if (run == 1)
	{
	  c1.close ();
	  c2.close ();
	};

				       // now merge the two and print the
				       // results
      c1.merge (c2);
      c1.print (logfile);
    };
}


int main ()
{
  deallog << std::setprecision (2);
  logfile << std::setprecision (2);
  deallog.attach(logfile);
  deallog.depth_console(0);
  deallog.threshold_double(1.e-10);

  merge_check ();
}

