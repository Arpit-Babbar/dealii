// ---------------------------------------------------------------------
//
// Copyright (C) 2006 - 2018 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------



// on a somewhat deformed cube, verify that if we push forward a bunch
// of points from the reference to the real cell and then call
// MappingFEField::transform_unit_to_real_cell that we get the same point as
// we had in the beginning.

#include <deal.II/base/utilities.h>

#include <deal.II/dofs/dof_handler.h>

#include <deal.II/fe/component_mask.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/mapping_fe_field.h>
#include <deal.II/fe/mapping_q1.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria.h>

#include <deal.II/numerics/vector_tools.h>

#include "../tests.h"



template <int dim, int spacedim>
void
test_real_to_unit_cell()
{
  deallog << "dim=" << dim << ", spacedim=" << spacedim << std::endl;

  Triangulation<dim, spacedim> triangulation;
  GridGenerator::hyper_cube(triangulation);

  const unsigned int      n_points = 4;
  std::vector<Point<dim>> unit_points(Utilities::fixed_power<dim>(n_points));

  switch (dim)
    {
      case 1:
        for (unsigned int x = 0; x < n_points; ++x)
          unit_points[x][0] = double(x) / double(n_points);
        break;

      case 2:
        for (unsigned int x = 0; x < n_points; ++x)
          for (unsigned int y = 0; y < n_points; ++y)
            {
              unit_points[y * n_points + x][0] = double(x) / double(n_points);
              unit_points[y * n_points + x][1] = double(y) / double(n_points);
            }
        break;

      case 3:
        for (unsigned int x = 0; x < n_points; ++x)
          for (unsigned int y = 0; y < n_points; ++y)
            for (unsigned int z = 0; z < n_points; ++z)
              {
                unit_points[z * n_points * n_points + y * n_points + x][0] =
                  double(x) / double(n_points);
                unit_points[z * n_points * n_points + y * n_points + x][1] =
                  double(y) / double(n_points);
                unit_points[z * n_points * n_points + y * n_points + x][2] =
                  double(z) / double(n_points);
              }
        break;
    }

  const FESystem<dim, spacedim> fesystem(FE_Q<dim, spacedim>(1),
                                         1,
                                         FE_Q<dim, spacedim>(2),
                                         spacedim);

  DoFHandler<dim, spacedim> dhq(triangulation);
  dhq.distribute_dofs(fesystem);
  Vector<double> eulerq(dhq.n_dofs());

  // Let's use only the last spacedim components
  ComponentMask mask(spacedim + 1, true);
  mask.set(0, false);

  VectorTools::get_position_vector(dhq, eulerq, mask);
  MappingFEField<dim, spacedim> map(dhq, eulerq, mask);

  typename Triangulation<dim, spacedim>::active_cell_iterator cell =
    triangulation.begin_active();

  // Move a vertex a little bit
  const unsigned int n_dx = 5;
  const double       dx   = 0.4 / n_dx;
  Point<spacedim>    direction;
  for (unsigned int j = 0; j < spacedim; ++j)
    direction[j] = dx;

  // in a loop, make the cell more
  // and more distorted
  for (unsigned int j = 0; j < n_dx; ++j)
    {
      deallog << "Vertex displacement: " << double(j) * direction << std::endl;
      cell->vertex(0) = double(j) * direction;

      for (unsigned int i = 0; i < unit_points.size(); ++i)
        {
          // for each of the points,
          // verify that if we apply
          // the forward map and then
          // pull back that we get
          // the same point again

          const Point<spacedim> p =
            map.transform_unit_to_real_cell(cell, unit_points[i]);
          const Point<dim> p_unit = map.transform_real_to_unit_cell(cell, p);

          Assert(unit_points[i].distance(p_unit) < 1e-10, ExcInternalError());
        }
    }

  deallog << "OK" << std::endl;
}


int
main()
{
  initlog();

  test_real_to_unit_cell<1, 1>();
  test_real_to_unit_cell<2, 2>();
  test_real_to_unit_cell<3, 3>();

  test_real_to_unit_cell<1, 2>();
  test_real_to_unit_cell<1, 3>();
  test_real_to_unit_cell<2, 3>();
}
