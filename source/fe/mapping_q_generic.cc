// ---------------------------------------------------------------------
//
// Copyright (C) 2000 - 2020 by the deal.II authors
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


#include <deal.II/base/array_view.h>
#include <deal.II/base/derivative_form.h>
#include <deal.II/base/memory_consumption.h>
#include <deal.II/base/qprojector.h>
#include <deal.II/base/quadrature.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/table.h>
#include <deal.II/base/tensor_product_polynomials.h>

#include <deal.II/fe/fe_dgq.h>
#include <deal.II/fe/fe_tools.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/mapping_q1.h>
#include <deal.II/fe/mapping_q_generic.h>

#include <deal.II/grid/grid_tools.h>
#include <deal.II/grid/manifold_lib.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/tria_iterator.h>

#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/tensor_product_matrix.h>

#include <deal.II/matrix_free/evaluation_template_factory.h>
#include <deal.II/matrix_free/shape_info.h>
#include <deal.II/matrix_free/tensor_product_kernels.h>

#include <boost/container/small_vector.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numeric>


DEAL_II_NAMESPACE_OPEN


namespace internal
{
  namespace MappingQ1
  {
    namespace
    {
      // These are left as templates on the spatial dimension (even though dim
      // == spacedim must be true for them to make sense) because templates are
      // expanded before the compiler eliminates code due to the 'if (dim ==
      // spacedim)' statement (see the body of the general
      // transform_real_to_unit_cell).
      template <int spacedim>
      Point<1>
      transform_real_to_unit_cell(
        const std::array<Point<spacedim>, GeometryInfo<1>::vertices_per_cell>
          &                    vertices,
        const Point<spacedim> &p)
      {
        Assert(spacedim == 1, ExcInternalError());
        return Point<1>((p[0] - vertices[0](0)) /
                        (vertices[1](0) - vertices[0](0)));
      }



      template <int spacedim>
      Point<2>
      transform_real_to_unit_cell(
        const std::array<Point<spacedim>, GeometryInfo<2>::vertices_per_cell>
          &                    vertices,
        const Point<spacedim> &p)
      {
        Assert(spacedim == 2, ExcInternalError());

        // For accuracy reasons, we do all arithmetic in extended precision
        // (long double). This has a noticeable effect on the hit rate for
        // borderline cases and thus makes the algorithm more robust.
        const long double x = p(0);
        const long double y = p(1);

        const long double x0 = vertices[0](0);
        const long double x1 = vertices[1](0);
        const long double x2 = vertices[2](0);
        const long double x3 = vertices[3](0);

        const long double y0 = vertices[0](1);
        const long double y1 = vertices[1](1);
        const long double y2 = vertices[2](1);
        const long double y3 = vertices[3](1);

        const long double a = (x1 - x3) * (y0 - y2) - (x0 - x2) * (y1 - y3);
        const long double b = -(x0 - x1 - x2 + x3) * y +
                              (x - 2 * x1 + x3) * y0 - (x - 2 * x0 + x2) * y1 -
                              (x - x1) * y2 + (x - x0) * y3;
        const long double c = (x0 - x1) * y - (x - x1) * y0 + (x - x0) * y1;

        const long double discriminant = b * b - 4 * a * c;
        // exit if the point is not in the cell (this is the only case where the
        // discriminant is negative)
        AssertThrow(
          discriminant > 0.0,
          (typename Mapping<spacedim, spacedim>::ExcTransformationFailed()));

        long double       eta1;
        long double       eta2;
        const long double sqrt_discriminant = std::sqrt(discriminant);
        // special case #1: if a is near-zero to make the discriminant exactly
        // equal b, then use the linear formula
        if (b != 0.0 && std::abs(b) == sqrt_discriminant)
          {
            eta1 = -c / b;
            eta2 = -c / b;
          }
        // special case #2: a is zero for parallelograms and very small for
        // near-parallelograms:
        else if (std::abs(a) < 1e-8 * std::abs(b))
          {
            // if both a and c are very small then the root should be near
            // zero: this first case will capture that
            eta1 = 2 * c / (-b - sqrt_discriminant);
            eta2 = 2 * c / (-b + sqrt_discriminant);
          }
        // finally, use the plain version:
        else
          {
            eta1 = (-b - sqrt_discriminant) / (2 * a);
            eta2 = (-b + sqrt_discriminant) / (2 * a);
          }
        // pick the one closer to the center of the cell.
        const long double eta =
          (std::abs(eta1 - 0.5) < std::abs(eta2 - 0.5)) ? eta1 : eta2;

        /*
         * There are two ways to compute xi from eta, but either one may have a
         * zero denominator.
         */
        const long double subexpr0 = -eta * x2 + x0 * (eta - 1);
        const long double xi_denominator0 =
          eta * x3 - x1 * (eta - 1) + subexpr0;
        const long double max_x =
          std::max(std::max(std::abs(x0), std::abs(x1)),
                   std::max(std::abs(x2), std::abs(x3)));

        if (std::abs(xi_denominator0) > 1e-10 * max_x)
          {
            const double xi = (x + subexpr0) / xi_denominator0;
            return {xi, static_cast<double>(eta)};
          }
        else
          {
            const long double max_y =
              std::max(std::max(std::abs(y0), std::abs(y1)),
                       std::max(std::abs(y2), std::abs(y3)));
            const long double subexpr1 = -eta * y2 + y0 * (eta - 1);
            const long double xi_denominator1 =
              eta * y3 - y1 * (eta - 1) + subexpr1;
            if (std::abs(xi_denominator1) > 1e-10 * max_y)
              {
                const double xi = (subexpr1 + y) / xi_denominator1;
                return {xi, static_cast<double>(eta)};
              }
            else // give up and try Newton iteration
              {
                AssertThrow(
                  false,
                  (typename Mapping<spacedim,
                                    spacedim>::ExcTransformationFailed()));
              }
          }
        // bogus return to placate compiler. It should not be possible to get
        // here.
        Assert(false, ExcInternalError());
        return {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
      }



      template <int spacedim>
      Point<3>
      transform_real_to_unit_cell(
        const std::array<Point<spacedim>, GeometryInfo<3>::vertices_per_cell>
          & /*vertices*/,
        const Point<spacedim> & /*p*/)
      {
        // It should not be possible to get here
        Assert(false, ExcInternalError());
        return Point<3>();
      }
    } // namespace
  }   // namespace MappingQ1
} // namespace internal



template <int dim, int spacedim>
MappingQGeneric<dim, spacedim>::InternalData::InternalData(
  const unsigned int polynomial_degree)
  : polynomial_degree(polynomial_degree)
  , n_shape_functions(Utilities::fixed_power<dim>(polynomial_degree + 1))
  , line_support_points(QGaussLobatto<1>(polynomial_degree + 1))
  , tensor_product_quadrature(false)
{}



template <int dim, int spacedim>
std::size_t
MappingQGeneric<dim, spacedim>::InternalData::memory_consumption() const
{
  return (
    Mapping<dim, spacedim>::InternalDataBase::memory_consumption() +
    MemoryConsumption::memory_consumption(shape_values) +
    MemoryConsumption::memory_consumption(shape_derivatives) +
    MemoryConsumption::memory_consumption(covariant) +
    MemoryConsumption::memory_consumption(contravariant) +
    MemoryConsumption::memory_consumption(unit_tangentials) +
    MemoryConsumption::memory_consumption(aux) +
    MemoryConsumption::memory_consumption(mapping_support_points) +
    MemoryConsumption::memory_consumption(cell_of_current_support_points) +
    MemoryConsumption::memory_consumption(volume_elements) +
    MemoryConsumption::memory_consumption(polynomial_degree) +
    MemoryConsumption::memory_consumption(n_shape_functions));
}


template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::InternalData::initialize(
  const UpdateFlags      update_flags,
  const Quadrature<dim> &q,
  const unsigned int     n_original_q_points)
{
  // store the flags in the internal data object so we can access them
  // in fill_fe_*_values()
  this->update_each = update_flags;

  const unsigned int n_q_points = q.size();

  const bool needs_higher_order_terms =
    this->update_each &
    (update_jacobian_pushed_forward_grads | update_jacobian_2nd_derivatives |
     update_jacobian_pushed_forward_2nd_derivatives |
     update_jacobian_3rd_derivatives |
     update_jacobian_pushed_forward_3rd_derivatives);

  if (this->update_each & update_covariant_transformation)
    covariant.resize(n_original_q_points);

  if (this->update_each & update_contravariant_transformation)
    contravariant.resize(n_original_q_points);

  if (this->update_each & update_volume_elements)
    volume_elements.resize(n_original_q_points);

  tensor_product_quadrature = q.is_tensor_product();

  // use of MatrixFree only for higher order elements and with more than one
  // point where tensor products do not make sense
  if (polynomial_degree < 2 || n_q_points == 1)
    tensor_product_quadrature = false;

  if (dim > 1)
    {
      // find out if the one-dimensional formula is the same
      // in all directions
      if (tensor_product_quadrature)
        {
          const std::array<Quadrature<1>, dim> quad_array =
            q.get_tensor_basis();
          for (unsigned int i = 1; i < dim && tensor_product_quadrature; ++i)
            {
              if (quad_array[i - 1].size() != quad_array[i].size())
                {
                  tensor_product_quadrature = false;
                  break;
                }
              else
                {
                  const std::vector<Point<1>> &points_1 =
                    quad_array[i - 1].get_points();
                  const std::vector<Point<1>> &points_2 =
                    quad_array[i].get_points();
                  const std::vector<double> &weights_1 =
                    quad_array[i - 1].get_weights();
                  const std::vector<double> &weights_2 =
                    quad_array[i].get_weights();
                  for (unsigned int j = 0; j < quad_array[i].size(); ++j)
                    {
                      if (std::abs(points_1[j][0] - points_2[j][0]) > 1.e-10 ||
                          std::abs(weights_1[j] - weights_2[j]) > 1.e-10)
                        {
                          tensor_product_quadrature = false;
                          break;
                        }
                    }
                }
            }

          if (tensor_product_quadrature)
            {
              // use a 1D FE_DGQ and adjust the hierarchic -> lexicographic
              // numbering manually (building an FE_Q<dim> is relatively
              // expensive due to constraints)
              const FE_DGQ<1> fe(polynomial_degree);
              shape_info.reinit(q.get_tensor_basis()[0], fe);
              shape_info.lexicographic_numbering =
                FETools::lexicographic_to_hierarchic_numbering<dim>(
                  polynomial_degree);
              shape_info.n_q_points = q.size();
              shape_info.dofs_per_component_on_cell =
                Utilities::pow(polynomial_degree + 1, dim);
            }
        }
    }

  // Only fill the big arrays on demand in case we cannot use the tensor
  // product quadrature code path
  if (dim == 1 || !tensor_product_quadrature || needs_higher_order_terms)
    {
      // see if we need the (transformation) shape function values
      // and/or gradients and resize the necessary arrays
      if (this->update_each & update_quadrature_points)
        shape_values.resize(n_shape_functions * n_q_points);

      if (this->update_each &
          (update_covariant_transformation |
           update_contravariant_transformation | update_JxW_values |
           update_boundary_forms | update_normal_vectors | update_jacobians |
           update_jacobian_grads | update_inverse_jacobians |
           update_jacobian_pushed_forward_grads |
           update_jacobian_2nd_derivatives |
           update_jacobian_pushed_forward_2nd_derivatives |
           update_jacobian_3rd_derivatives |
           update_jacobian_pushed_forward_3rd_derivatives))
        shape_derivatives.resize(n_shape_functions * n_q_points);

      if (this->update_each &
          (update_jacobian_grads | update_jacobian_pushed_forward_grads))
        shape_second_derivatives.resize(n_shape_functions * n_q_points);

      if (this->update_each & (update_jacobian_2nd_derivatives |
                               update_jacobian_pushed_forward_2nd_derivatives))
        shape_third_derivatives.resize(n_shape_functions * n_q_points);

      if (this->update_each & (update_jacobian_3rd_derivatives |
                               update_jacobian_pushed_forward_3rd_derivatives))
        shape_fourth_derivatives.resize(n_shape_functions * n_q_points);

      // now also fill the various fields with their correct values
      compute_shape_function_values(q.get_points());
    }
}



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::InternalData::initialize_face(
  const UpdateFlags      update_flags,
  const Quadrature<dim> &q,
  const unsigned int     n_original_q_points)
{
  initialize(update_flags, q, n_original_q_points);

  if (dim > 1 && tensor_product_quadrature)
    {
      constexpr unsigned int facedim = dim - 1;
      const FE_DGQ<1>        fe(polynomial_degree);
      shape_info.reinit(q.get_tensor_basis()[0], fe);
      shape_info.lexicographic_numbering =
        FETools::lexicographic_to_hierarchic_numbering<facedim>(
          polynomial_degree);
      shape_info.n_q_points = n_original_q_points;
      shape_info.dofs_per_component_on_cell =
        Utilities::pow(polynomial_degree + 1, dim);
    }

  if (dim > 1)
    {
      if (this->update_each &
          (update_boundary_forms | update_normal_vectors | update_jacobians |
           update_JxW_values | update_inverse_jacobians))
        {
          aux.resize(dim - 1,
                     std::vector<Tensor<1, spacedim>>(n_original_q_points));

          // Compute tangentials to the unit cell.
          for (const unsigned int i : GeometryInfo<dim>::face_indices())
            {
              unit_tangentials[i].resize(n_original_q_points);
              std::fill(unit_tangentials[i].begin(),
                        unit_tangentials[i].end(),
                        GeometryInfo<dim>::unit_tangential_vectors[i][0]);
              if (dim > 2)
                {
                  unit_tangentials[GeometryInfo<dim>::faces_per_cell + i]
                    .resize(n_original_q_points);
                  std::fill(
                    unit_tangentials[GeometryInfo<dim>::faces_per_cell + i]
                      .begin(),
                    unit_tangentials[GeometryInfo<dim>::faces_per_cell + i]
                      .end(),
                    GeometryInfo<dim>::unit_tangential_vectors[i][1]);
                }
            }
        }
    }
}



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::InternalData::compute_shape_function_values(
  const std::vector<Point<dim>> &unit_points)
{
  const unsigned int n_points = unit_points.size();

  // Construct the tensor product polynomials used as shape functions for
  // the Qp mapping of cells at the boundary.
  const TensorProductPolynomials<dim> tensor_pols(
    Polynomials::generate_complete_Lagrange_basis(
      line_support_points.get_points()));
  Assert(n_shape_functions == tensor_pols.n(), ExcInternalError());

  // then also construct the mapping from lexicographic to the Qp shape
  // function numbering
  const std::vector<unsigned int> renumber =
    FETools::hierarchic_to_lexicographic_numbering<dim>(polynomial_degree);

  std::vector<double>         values;
  std::vector<Tensor<1, dim>> grads;
  if (shape_values.size() != 0)
    {
      Assert(shape_values.size() == n_shape_functions * n_points,
             ExcInternalError());
      values.resize(n_shape_functions);
    }
  if (shape_derivatives.size() != 0)
    {
      Assert(shape_derivatives.size() == n_shape_functions * n_points,
             ExcInternalError());
      grads.resize(n_shape_functions);
    }

  std::vector<Tensor<2, dim>> grad2;
  if (shape_second_derivatives.size() != 0)
    {
      Assert(shape_second_derivatives.size() == n_shape_functions * n_points,
             ExcInternalError());
      grad2.resize(n_shape_functions);
    }

  std::vector<Tensor<3, dim>> grad3;
  if (shape_third_derivatives.size() != 0)
    {
      Assert(shape_third_derivatives.size() == n_shape_functions * n_points,
             ExcInternalError());
      grad3.resize(n_shape_functions);
    }

  std::vector<Tensor<4, dim>> grad4;
  if (shape_fourth_derivatives.size() != 0)
    {
      Assert(shape_fourth_derivatives.size() == n_shape_functions * n_points,
             ExcInternalError());
      grad4.resize(n_shape_functions);
    }


  if (shape_values.size() != 0 || shape_derivatives.size() != 0 ||
      shape_second_derivatives.size() != 0 ||
      shape_third_derivatives.size() != 0 ||
      shape_fourth_derivatives.size() != 0)
    for (unsigned int point = 0; point < n_points; ++point)
      {
        tensor_pols.evaluate(
          unit_points[point], values, grads, grad2, grad3, grad4);

        if (shape_values.size() != 0)
          for (unsigned int i = 0; i < n_shape_functions; ++i)
            shape(point, i) = values[renumber[i]];

        if (shape_derivatives.size() != 0)
          for (unsigned int i = 0; i < n_shape_functions; ++i)
            derivative(point, i) = grads[renumber[i]];

        if (shape_second_derivatives.size() != 0)
          for (unsigned int i = 0; i < n_shape_functions; ++i)
            second_derivative(point, i) = grad2[renumber[i]];

        if (shape_third_derivatives.size() != 0)
          for (unsigned int i = 0; i < n_shape_functions; ++i)
            third_derivative(point, i) = grad3[renumber[i]];

        if (shape_fourth_derivatives.size() != 0)
          for (unsigned int i = 0; i < n_shape_functions; ++i)
            fourth_derivative(point, i) = grad4[renumber[i]];
      }
}



namespace internal
{
  namespace MappingQGenericImplementation
  {
    namespace
    {
      /**
       * This function is needed by the constructor of
       * <tt>MappingQ<dim,spacedim></tt> for <tt>dim=</tt> 2 and 3.
       *
       * For the definition of the @p support_point_weights_on_quad please
       * refer to the description of TransfiniteInterpolationManifold.
       */
      dealii::Table<2, double>
      compute_support_point_weights_on_quad(
        const unsigned int polynomial_degree)
      {
        dealii::Table<2, double> loqvs;

        // we are asked to compute weights for interior support points, but
        // there are no interior points if degree==1
        if (polynomial_degree == 1)
          return loqvs;

        const unsigned int M          = polynomial_degree - 1;
        const unsigned int n_inner_2d = M * M;
        const unsigned int n_outer_2d = 4 + 4 * M;

        // set the weights of transfinite interpolation
        loqvs.reinit(n_inner_2d, n_outer_2d);
        QGaussLobatto<2> gl(polynomial_degree + 1);
        for (unsigned int i = 0; i < M; ++i)
          for (unsigned int j = 0; j < M; ++j)
            {
              const Point<2> p =
                gl.point((i + 1) * (polynomial_degree + 1) + (j + 1));
              const unsigned int index_table = i * M + j;
              for (unsigned int v = 0; v < 4; ++v)
                loqvs(index_table, v) =
                  -GeometryInfo<2>::d_linear_shape_function(p, v);
              loqvs(index_table, 4 + i)         = 1. - p[0];
              loqvs(index_table, 4 + i + M)     = p[0];
              loqvs(index_table, 4 + j + 2 * M) = 1. - p[1];
              loqvs(index_table, 4 + j + 3 * M) = p[1];
            }

        // the sum of weights of the points at the outer rim should be one.
        // check this
        for (unsigned int unit_point = 0; unit_point < n_inner_2d; ++unit_point)
          Assert(std::fabs(std::accumulate(loqvs[unit_point].begin(),
                                           loqvs[unit_point].end(),
                                           0.) -
                           1) < 1e-13 * polynomial_degree,
                 ExcInternalError());

        return loqvs;
      }



      /**
       * This function is needed by the constructor of <tt>MappingQ<3></tt>.
       *
       * For the definition of the @p support_point_weights_on_quad please
       * refer to the description of TransfiniteInterpolationManifold.
       */
      dealii::Table<2, double>
      compute_support_point_weights_on_hex(const unsigned int polynomial_degree)
      {
        dealii::Table<2, double> lohvs;

        // we are asked to compute weights for interior support points, but
        // there are no interior points if degree==1
        if (polynomial_degree == 1)
          return lohvs;

        const unsigned int M = polynomial_degree - 1;

        const unsigned int n_inner = Utilities::fixed_power<3>(M);
        const unsigned int n_outer = 8 + 12 * M + 6 * M * M;

        // set the weights of transfinite interpolation
        lohvs.reinit(n_inner, n_outer);
        QGaussLobatto<3> gl(polynomial_degree + 1);
        for (unsigned int i = 0; i < M; ++i)
          for (unsigned int j = 0; j < M; ++j)
            for (unsigned int k = 0; k < M; ++k)
              {
                const Point<3>     p = gl.point((i + 1) * (M + 2) * (M + 2) +
                                            (j + 1) * (M + 2) + (k + 1));
                const unsigned int index_table = i * M * M + j * M + k;

                // vertices
                for (unsigned int v = 0; v < 8; ++v)
                  lohvs(index_table, v) =
                    GeometryInfo<3>::d_linear_shape_function(p, v);

                // lines
                {
                  constexpr std::array<unsigned int, 4> line_coordinates_y(
                    {{0, 1, 4, 5}});
                  const Point<2> py(p[0], p[2]);
                  for (unsigned int l = 0; l < 4; ++l)
                    lohvs(index_table, 8 + line_coordinates_y[l] * M + j) =
                      -GeometryInfo<2>::d_linear_shape_function(py, l);
                }

                {
                  constexpr std::array<unsigned int, 4> line_coordinates_x(
                    {{2, 3, 6, 7}});
                  const Point<2> px(p[1], p[2]);
                  for (unsigned int l = 0; l < 4; ++l)
                    lohvs(index_table, 8 + line_coordinates_x[l] * M + k) =
                      -GeometryInfo<2>::d_linear_shape_function(px, l);
                }

                {
                  constexpr std::array<unsigned int, 4> line_coordinates_z(
                    {{8, 9, 10, 11}});
                  const Point<2> pz(p[0], p[1]);
                  for (unsigned int l = 0; l < 4; ++l)
                    lohvs(index_table, 8 + line_coordinates_z[l] * M + i) =
                      -GeometryInfo<2>::d_linear_shape_function(pz, l);
                }

                // quads
                lohvs(index_table, 8 + 12 * M + 0 * M * M + i * M + j) =
                  1. - p[0];
                lohvs(index_table, 8 + 12 * M + 1 * M * M + i * M + j) = p[0];
                lohvs(index_table, 8 + 12 * M + 2 * M * M + k * M + i) =
                  1. - p[1];
                lohvs(index_table, 8 + 12 * M + 3 * M * M + k * M + i) = p[1];
                lohvs(index_table, 8 + 12 * M + 4 * M * M + j * M + k) =
                  1. - p[2];
                lohvs(index_table, 8 + 12 * M + 5 * M * M + j * M + k) = p[2];
              }

        // the sum of weights of the points at the outer rim should be one.
        // check this
        for (unsigned int unit_point = 0; unit_point < n_inner; ++unit_point)
          Assert(std::fabs(std::accumulate(lohvs[unit_point].begin(),
                                           lohvs[unit_point].end(),
                                           0.) -
                           1) < 1e-13 * polynomial_degree,
                 ExcInternalError());

        return lohvs;
      }



      /**
       * This function collects the output of
       * compute_support_point_weights_on_{quad,hex} in a single data structure.
       */
      std::vector<dealii::Table<2, double>>
      compute_support_point_weights_perimeter_to_interior(
        const unsigned int polynomial_degree,
        const unsigned int dim)
      {
        Assert(dim > 0 && dim <= 3, ExcImpossibleInDim(dim));
        std::vector<dealii::Table<2, double>> output(dim);
        if (polynomial_degree <= 1)
          return output;

        // fill the 1D interior weights
        QGaussLobatto<1> quadrature(polynomial_degree + 1);
        output[0].reinit(polynomial_degree - 1,
                         GeometryInfo<1>::vertices_per_cell);
        for (unsigned int q = 0; q < polynomial_degree - 1; ++q)
          for (const unsigned int i : GeometryInfo<1>::vertex_indices())
            output[0](q, i) =
              GeometryInfo<1>::d_linear_shape_function(quadrature.point(q + 1),
                                                       i);

        if (dim > 1)
          output[1] = compute_support_point_weights_on_quad(polynomial_degree);

        if (dim > 2)
          output[2] = compute_support_point_weights_on_hex(polynomial_degree);

        return output;
      }

      /**
       * Collects all interior points for the various dimensions.
       */
      template <int dim>
      dealii::Table<2, double>
      compute_support_point_weights_cell(const unsigned int polynomial_degree)
      {
        Assert(dim > 0 && dim <= 3, ExcImpossibleInDim(dim));
        if (polynomial_degree <= 1)
          return dealii::Table<2, double>();

        QGaussLobatto<dim>              quadrature(polynomial_degree + 1);
        const std::vector<unsigned int> h2l =
          FETools::hierarchic_to_lexicographic_numbering<dim>(
            polynomial_degree);

        dealii::Table<2, double> output(quadrature.size() -
                                          GeometryInfo<dim>::vertices_per_cell,
                                        GeometryInfo<dim>::vertices_per_cell);
        for (unsigned int q = 0; q < output.size(0); ++q)
          for (const unsigned int i : GeometryInfo<dim>::vertex_indices())
            output(q, i) = GeometryInfo<dim>::d_linear_shape_function(
              quadrature.point(h2l[q + GeometryInfo<dim>::vertices_per_cell]),
              i);

        return output;
      }



      /**
       * Using the relative weights of the shape functions evaluated at
       * one point on the reference cell (and stored in data.shape_values
       * and accessed via data.shape(0,i)) and the locations of mapping
       * support points (stored in data.mapping_support_points), compute
       * the mapped location of that point in real space.
       */
      template <int dim, int spacedim>
      Point<spacedim>
      compute_mapped_location_of_point(
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &data)
      {
        AssertDimension(data.shape_values.size(),
                        data.mapping_support_points.size());

        // use now the InternalData to compute the point in real space.
        Point<spacedim> p_real;
        for (unsigned int i = 0; i < data.mapping_support_points.size(); ++i)
          p_real += data.mapping_support_points[i] * data.shape(0, i);

        return p_real;
      }



      /**
       * Using the given 1D polynomial basis and the position of the mapping
       * support points, compute the mapped location of that point in real
       * space. This function is much faster than the other implementation
       * going via the expanded shape functions in InternalData because it
       * directly works in the tensor product form. This also gives the
       * derivative almost for free (less than 2x the cost of simply the
       * values), so we always compute it.
       */
      template <int dim, int spacedim>
      std::pair<Point<spacedim>, Tensor<2, spacedim>>
      compute_mapped_location_of_point(
        const std::vector<Point<spacedim>> &                points,
        const std::vector<Polynomials::Polynomial<double>> &poly,
        const std::vector<unsigned int> &                   renumber,
        const Point<dim> &                                  p)
      {
        static_assert(dim >= 1 && dim <= 3, "Only dim=1,2,3 implemented");

        const unsigned int n_shapes = poly.size();

        // shortcut for linear interpolation to speed up evaluation
        if (n_shapes == 2)
          {
            if (dim == 1)
              {
                Tensor<2, spacedim> derivative;
                derivative[0] = points[1] - points[0];
                return std::make_pair((1. - p[0]) * points[0] +
                                        p[0] * points[1],
                                      derivative);
              }
            else if (dim == 2)
              {
                const double          x0 = 1. - p[0], x1 = p[0];
                const Point<spacedim> tmp0   = x0 * points[0] + x1 * points[1];
                const Point<spacedim> tmp1   = x0 * points[2] + x1 * points[3];
                const Point<spacedim> mapped = (1. - p[1]) * tmp0 + p[1] * tmp1;
                Tensor<2, spacedim>   derivative;
                derivative[0] = (1. - p[1]) * (points[1] - points[0]) +
                                p[1] * (points[3] - points[2]);
                derivative[1] = tmp1 - tmp0;
                return std::make_pair(mapped, transpose(derivative));
              }
            else if (dim == 3)
              {
                const double x0 = 1. - p[0], x1 = p[0], y0 = 1. - p[1],
                             y1 = p[1], z0 = 1. - p[2], z1 = p[2];
                const Point<spacedim> tmp0   = x0 * points[0] + x1 * points[1];
                const Point<spacedim> tmp1   = x0 * points[2] + x1 * points[3];
                const Point<spacedim> tmpy0  = y0 * tmp0 + y1 * tmp1;
                const Point<spacedim> tmp2   = x0 * points[4] + x1 * points[5];
                const Point<spacedim> tmp3   = x0 * points[6] + x1 * points[7];
                const Point<spacedim> tmpy1  = y0 * tmp2 + y1 * tmp3;
                const Point<spacedim> mapped = z0 * tmpy0 + z1 * tmpy1;
                Tensor<2, spacedim>   derivative;
                derivative[2] = tmpy1 - tmpy0;
                derivative[1] = z0 * (tmp1 - tmp0) + z1 * (tmp3 - tmp2);
                derivative[0] = z0 * (y0 * (points[1] - points[0]) +
                                      y1 * (points[3] - points[2])) +
                                z1 * (y0 * (points[5] - points[4]) +
                                      y1 * (points[7] - points[6]));
                return std::make_pair(mapped, transpose(derivative));
              }
          }

        // Put up to 32 shape functions per dimension on stack, else on heap
        boost::container::small_vector<double, 64 * dim> shapes(2 * dim *
                                                                n_shapes);

        // Evaluate 1D polynomials and their derivatives
        for (unsigned int d = 0; d < dim; ++d)
          for (unsigned int i = 0; i < n_shapes; ++i)
            poly[i].value(p[d], 1, shapes.data() + 2 * (d * n_shapes + i));

        // Go through the tensor product of shape functions and interpolate
        // with optimal algorithm
        std::pair<Point<spacedim>, Tensor<2, spacedim>> result;
        for (unsigned int i2 = 0, i = 0; i2 < (dim > 2 ? n_shapes : 1); ++i2)
          {
            Point<spacedim> value_y, deriv_x, deriv_y;
            for (unsigned int i1 = 0; i1 < (dim > 1 ? n_shapes : 1); ++i1)
              {
                // interpolation + derivative x direction
                Point<spacedim> value, deriv;
                for (unsigned int i0 = 0; i0 < n_shapes; ++i0, ++i)
                  {
                    value += shapes[2 * i0] * points[renumber[i]];
                    deriv += shapes[2 * i0 + 1] * points[renumber[i]];
                  }

                // interpolation + derivative in y direction
                if (dim > 1)
                  {
                    value_y += value * shapes[2 * n_shapes + 2 * i1];
                    deriv_x += deriv * shapes[2 * n_shapes + 2 * i1];
                    deriv_y += value * shapes[2 * n_shapes + 2 * i1 + 1];
                  }
                else
                  {
                    result.first     = value;
                    result.second[0] = deriv;
                  }
              }
            if (dim == 3)
              {
                // interpolation + derivative in z direction
                result.first += value_y * shapes[4 * n_shapes + 2 * i2];
                for (unsigned int d = 0; d < spacedim; ++d)
                  {
                    result.second[d][0] +=
                      deriv_x[d] * shapes[4 * n_shapes + 2 * i2];
                    result.second[d][1] +=
                      deriv_y[d] * shapes[4 * n_shapes + 2 * i2];
                    result.second[d][2] +=
                      value_y[d] * shapes[4 * n_shapes + 2 * i2 + 1];
                  }
              }
            else if (dim == 2)
              {
                result.first = value_y;
                for (unsigned int d = 0; d < spacedim; ++d)
                  {
                    result.second[d][0] = deriv_x[d];
                    result.second[d][1] = deriv_y[d];
                  }
              }
          }

        return result;
      }



      /**
       * Implementation of transform_real_to_unit_cell for dim==spacedim
       */
      template <int dim, int spacedim>
      Point<dim>
      do_transform_real_to_unit_cell_internal(
        const Point<spacedim> &                             p,
        const Point<dim> &                                  initial_p_unit,
        const std::vector<Point<spacedim>> &                points,
        const std::vector<Polynomials::Polynomial<double>> &polynomials_1d,
        const std::vector<unsigned int> &                   renumber)
      {
        AssertDimension(points.size(),
                        Utilities::pow(polynomials_1d.size(), dim));

        // Newton iteration to solve
        //    f(x)=p(x)-p=0
        // where we are looking for 'x' and p(x) is the forward transformation
        // from unit to real cell. We solve this using a Newton iteration
        //    x_{n+1}=x_n-[f'(x)]^{-1}f(x)
        // The start value is set to be the linear approximation to the cell

        // The shape values and derivatives of the mapping at this point are
        // previously computed.

        Point<dim>                                      p_unit = initial_p_unit;
        std::pair<Point<spacedim>, Tensor<2, spacedim>> p_real =
          compute_mapped_location_of_point(points,
                                           polynomials_1d,
                                           renumber,
                                           p_unit);

        Tensor<1, spacedim> f = p_real.first - p;

        // early out if we already have our point
        if (f.norm_square() < 1e-24 * p_real.second.norm_square())
          return p_unit;

        // we need to compare the position of the computed p(x) against the
        // given point 'p'. We will terminate the iteration and return 'x' if
        // they are less than eps apart. The question is how to choose eps --
        // or, put maybe more generally: in which norm we want these 'p' and
        // 'p(x)' to be eps apart.
        //
        // the question is difficult since we may have to deal with very
        // elongated cells where we may achieve 1e-12*h for the distance of
        // these two points in the 'long' direction, but achieving this
        // tolerance in the 'short' direction of the cell may not be possible
        //
        // what we do instead is then to terminate iterations if
        //    \| p(x) - p \|_A < eps
        // where the A-norm is somehow induced by the transformation of the
        // cell. in particular, we want to measure distances relative to the
        // sizes of the cell in its principal directions.
        //
        // to define what exactly A should be, note that to first order we have
        // the following (assuming that x* is the solution of the problem, i.e.,
        // p(x*)=p):
        //    p(x) - p = p(x) - p(x*)
        //             = -grad p(x) * (x*-x) + higher order terms
        // This suggest to measure with a norm that corresponds to
        //    A = {[grad p(x]^T [grad p(x)]}^{-1}
        // because then
        //    \| p(x) - p \|_A  \approx  \| x - x* \|
        // Consequently, we will try to enforce that
        //    \| p(x) - p \|_A  =  \| f \|  <=  eps
        //
        // Note that using this norm is a bit dangerous since the norm changes
        // in every iteration (A isn't fixed by depending on xk). However, if
        // the cell is not too deformed (it may be stretched, but not twisted)
        // then the mapping is almost linear and A is indeed constant or
        // nearly so.
        const double       eps                    = 1.e-11;
        const unsigned int newton_iteration_limit = 20;

        Point<dim> invalid_point;
        invalid_point[0] = std::numeric_limits<double>::infinity();

        unsigned int newton_iteration = 0;
        double       last_f_weighted_norm_square;
        do
          {
#ifdef DEBUG_TRANSFORM_REAL_TO_UNIT_CELL
            std::cout << "Newton iteration " << newton_iteration << std::endl;
#endif

            // f'(x)
            const Tensor<2, spacedim> &df = p_real.second;

            // Solve  [f'(x)]d=f(x)
            if (determinant(df) <= 0)
              return invalid_point;

            const Tensor<2, spacedim> df_inverse = invert(df);
            const Tensor<1, spacedim> delta      = df_inverse * f;

#ifdef DEBUG_TRANSFORM_REAL_TO_UNIT_CELL
            std::cout << "   delta=" << delta << std::endl;
#endif

            // do a line search
            double step_length = 1;
            do
              {
                // update of p_unit. The spacedim-th component of transformed
                // point is simply ignored in codimension one case. When this
                // component is not zero, then we are projecting the point to
                // the surface or curve identified by the cell.
                Point<dim> p_unit_trial = p_unit;
                for (unsigned int i = 0; i < dim; ++i)
                  p_unit_trial[i] -= step_length * delta[i];

                // shape values and derivatives
                // at new p_unit point
                std::pair<Point<spacedim>, Tensor<2, spacedim>> p_real_trial =
                  compute_mapped_location_of_point(points,
                                                   polynomials_1d,
                                                   renumber,
                                                   p_unit_trial);
                const Tensor<1, spacedim> f_trial = p_real_trial.first - p;

#ifdef DEBUG_TRANSFORM_REAL_TO_UNIT_CELL
                std::cout << "     step_length=" << step_length << std::endl
                          << "       ||f ||   =" << f.norm() << std::endl
                          << "       ||f*||   =" << f_trial.norm() << std::endl
                          << "       ||f*||_A ="
                          << (df_inverse * f_trial).norm() << std::endl;
#endif

                // see if we are making progress with the current step length
                // and if not, reduce it by a factor of two and try again
                //
                // strictly speaking, we should probably use the same norm as we
                // use for the outer algorithm. in practice, line search is just
                // a crutch to find a "reasonable" step length, and so using the
                // l2 norm is probably just fine
                if (f_trial.norm_square() < f.norm_square())
                  {
                    p_real = p_real_trial;
                    p_unit = p_unit_trial;
                    f      = f_trial;
                    break;
                  }
                else if (step_length > 0.05)
                  step_length /= 2;
                else
                  return invalid_point;
              }
            while (true);

            ++newton_iteration;
            if (newton_iteration > newton_iteration_limit)
              return invalid_point;
            last_f_weighted_norm_square = (df_inverse * f).norm_square();
          }
        while (last_f_weighted_norm_square > eps * eps);

        return p_unit;
      }



      /**
       * Implementation of transform_real_to_unit_cell for dim==spacedim-1
       */
      template <int dim>
      Point<dim>
      do_transform_real_to_unit_cell_internal_codim1(
        const typename dealii::Triangulation<dim, dim + 1>::cell_iterator &cell,
        const Point<dim + 1> &                                             p,
        const Point<dim> &initial_p_unit,
        typename dealii::MappingQGeneric<dim, dim + 1>::InternalData &mdata)
      {
        const unsigned int spacedim = dim + 1;

        const unsigned int n_shapes = mdata.shape_values.size();
        (void)n_shapes;
        Assert(n_shapes != 0, ExcInternalError());
        Assert(mdata.shape_derivatives.size() == n_shapes, ExcInternalError());
        Assert(mdata.shape_second_derivatives.size() == n_shapes,
               ExcInternalError());

        std::vector<Point<spacedim>> &points = mdata.mapping_support_points;
        Assert(points.size() == n_shapes, ExcInternalError());

        Point<spacedim> p_minus_F;

        Tensor<1, spacedim> DF[dim];
        Tensor<1, spacedim> D2F[dim][dim];

        Point<dim>     p_unit = initial_p_unit;
        Point<dim>     f;
        Tensor<2, dim> df;

        // Evaluate first and second derivatives
        mdata.compute_shape_function_values(std::vector<Point<dim>>(1, p_unit));

        for (unsigned int k = 0; k < mdata.n_shape_functions; ++k)
          {
            const Tensor<1, dim> & grad_phi_k = mdata.derivative(0, k);
            const Tensor<2, dim> & hessian_k  = mdata.second_derivative(0, k);
            const Point<spacedim> &point_k    = points[k];

            for (unsigned int j = 0; j < dim; ++j)
              {
                DF[j] += grad_phi_k[j] * point_k;
                for (unsigned int l = 0; l < dim; ++l)
                  D2F[j][l] += hessian_k[j][l] * point_k;
              }
          }

        p_minus_F = p;
        p_minus_F -= compute_mapped_location_of_point<dim, spacedim>(mdata);


        for (unsigned int j = 0; j < dim; ++j)
          f[j] = DF[j] * p_minus_F;

        for (unsigned int j = 0; j < dim; ++j)
          {
            f[j] = DF[j] * p_minus_F;
            for (unsigned int l = 0; l < dim; ++l)
              df[j][l] = -DF[j] * DF[l] + D2F[j][l] * p_minus_F;
          }


        const double       eps        = 1.e-12 * cell->diameter();
        const unsigned int loop_limit = 10;

        unsigned int loop = 0;

        while (f.norm() > eps && loop++ < loop_limit)
          {
            // Solve  [df(x)]d=f(x)
            const Tensor<1, dim> d =
              invert(df) * static_cast<const Tensor<1, dim> &>(f);
            p_unit -= d;

            for (unsigned int j = 0; j < dim; ++j)
              {
                DF[j].clear();
                for (unsigned int l = 0; l < dim; ++l)
                  D2F[j][l].clear();
              }

            mdata.compute_shape_function_values(
              std::vector<Point<dim>>(1, p_unit));

            for (unsigned int k = 0; k < mdata.n_shape_functions; ++k)
              {
                const Tensor<1, dim> &grad_phi_k = mdata.derivative(0, k);
                const Tensor<2, dim> &hessian_k = mdata.second_derivative(0, k);
                const Point<spacedim> &point_k  = points[k];

                for (unsigned int j = 0; j < dim; ++j)
                  {
                    DF[j] += grad_phi_k[j] * point_k;
                    for (unsigned int l = 0; l < dim; ++l)
                      D2F[j][l] += hessian_k[j][l] * point_k;
                  }
              }

            // TODO: implement a line search here in much the same way as for
            // the corresponding function above that does so for dim==spacedim
            p_minus_F = p;
            p_minus_F -= compute_mapped_location_of_point<dim, spacedim>(mdata);

            for (unsigned int j = 0; j < dim; ++j)
              {
                f[j] = DF[j] * p_minus_F;
                for (unsigned int l = 0; l < dim; ++l)
                  df[j][l] = -DF[j] * DF[l] + D2F[j][l] * p_minus_F;
              }
          }


        // Here we check that in the last execution of while the first
        // condition was already wrong, meaning the residual was below
        // eps. Only if the first condition failed, loop will have been
        // increased and tested, and thus have reached the limit.
        AssertThrow(
          loop < loop_limit,
          (typename Mapping<dim, spacedim>::ExcTransformationFailed()));

        return p_unit;
      }

      /**
       * In case the quadrature formula is a tensor product, this is a
       * replacement for maybe_compute_q_points(), maybe_update_Jacobians() and
       * maybe_update_jacobian_grads()
       */
      template <int dim, int spacedim>
      void
      maybe_update_q_points_Jacobians_and_grads_tensor(
        const CellSimilarity::Similarity cell_similarity,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &                                            data,
        std::vector<Point<spacedim>> &                 quadrature_points,
        std::vector<DerivativeForm<2, dim, spacedim>> &jacobian_grads)
      {
        const UpdateFlags update_flags = data.update_each;

        const unsigned int     n_shape_values = data.n_shape_functions;
        const unsigned int     n_q_points     = data.shape_info.n_q_points;
        constexpr unsigned int n_lanes        = VectorizedArray<double>::size();
        constexpr unsigned int n_comp         = 1 + (spacedim - 1) / n_lanes;
        constexpr unsigned int n_hessians     = (dim * (dim + 1)) / 2;

        EvaluationFlags::EvaluationFlags evaluation_flag =
          (update_flags & update_quadrature_points ? EvaluationFlags::values :
                                                     EvaluationFlags::nothing) |
          ((cell_similarity != CellSimilarity::translation) &&
               (update_flags & update_contravariant_transformation) ?
             EvaluationFlags::gradients :
             EvaluationFlags::nothing) |
          ((cell_similarity != CellSimilarity::translation) &&
               (update_flags & update_jacobian_grads) ?
             EvaluationFlags::hessians :
             EvaluationFlags::nothing);

        Assert(!(evaluation_flag & EvaluationFlags::values) || n_q_points > 0,
               ExcInternalError());
        Assert(!(evaluation_flag & EvaluationFlags::values) ||
                 n_q_points == quadrature_points.size(),
               ExcDimensionMismatch(n_q_points, quadrature_points.size()));
        Assert(!(evaluation_flag & EvaluationFlags::gradients) ||
                 data.n_shape_functions > 0,
               ExcInternalError());
        Assert(!(evaluation_flag & EvaluationFlags::gradients) ||
                 n_q_points == data.contravariant.size(),
               ExcDimensionMismatch(n_q_points, data.contravariant.size()));
        Assert(!(evaluation_flag & EvaluationFlags::hessians) ||
                 n_q_points == jacobian_grads.size(),
               ExcDimensionMismatch(n_q_points, jacobian_grads.size()));

        // shortcut in case we have an identity interpolation and only request
        // the quadrature points
        if (evaluation_flag == EvaluationFlags::values &&
            data.shape_info.element_type ==
              internal::MatrixFreeFunctions::tensor_symmetric_collocation)
          {
            for (unsigned int q = 0; q < n_q_points; ++q)
              quadrature_points[q] =
                data.mapping_support_points[data.shape_info
                                              .lexicographic_numbering[q]];
            return;
          }

        // prepare arrays
        if (evaluation_flag != EvaluationFlags::nothing)
          {
            data.values_dofs.resize(n_comp * n_shape_values);
            data.values_quad.resize(n_comp * n_q_points);
            data.gradients_quad.resize(n_comp * n_q_points * dim);
            data.scratch.resize(2 * std::max(n_q_points, n_shape_values));

            if (evaluation_flag & EvaluationFlags::hessians)
              data.hessians_quad.resize(n_comp * n_q_points * n_hessians);

            const std::vector<unsigned int> &renumber_to_lexicographic =
              data.shape_info.lexicographic_numbering;
            for (unsigned int i = 0; i < n_shape_values; ++i)
              for (unsigned int d = 0; d < spacedim; ++d)
                {
                  const unsigned int in_comp  = d % n_lanes;
                  const unsigned int out_comp = d / n_lanes;
                  data.values_dofs[out_comp * n_shape_values + i][in_comp] =
                    data
                      .mapping_support_points[renumber_to_lexicographic[i]][d];
                }

            // do the actual tensorized evaluation
            internal::FEEvaluationFactory<
              dim,
              double,
              VectorizedArray<double>>::evaluate(n_comp,
                                                 evaluation_flag,
                                                 data.shape_info,
                                                 data.values_dofs.begin(),
                                                 data.values_quad.begin(),
                                                 data.gradients_quad.begin(),
                                                 data.hessians_quad.begin(),
                                                 data.scratch.begin());
          }

        // do the postprocessing
        if (evaluation_flag & EvaluationFlags::values)
          {
            for (unsigned int out_comp = 0; out_comp < n_comp; ++out_comp)
              for (unsigned int i = 0; i < n_q_points; ++i)
                for (unsigned int in_comp = 0;
                     in_comp < n_lanes &&
                     in_comp < spacedim - out_comp * n_lanes;
                     ++in_comp)
                  quadrature_points[i][out_comp * n_lanes + in_comp] =
                    data.values_quad[out_comp * n_q_points + i][in_comp];
          }

        if (evaluation_flag & EvaluationFlags::gradients)
          {
            std::fill(data.contravariant.begin(),
                      data.contravariant.end(),
                      DerivativeForm<1, dim, spacedim>());
            // We need to reinterpret the data after evaluate has been applied.
            for (unsigned int out_comp = 0; out_comp < n_comp; ++out_comp)
              for (unsigned int point = 0; point < n_q_points; ++point)
                for (unsigned int j = 0; j < dim; ++j)
                  for (unsigned int in_comp = 0;
                       in_comp < n_lanes &&
                       in_comp < spacedim - out_comp * n_lanes;
                       ++in_comp)
                    {
                      const unsigned int total_number = point * dim + j;
                      const unsigned int new_comp  = total_number / n_q_points;
                      const unsigned int new_point = total_number % n_q_points;
                      data.contravariant[new_point][out_comp * n_lanes +
                                                    in_comp][new_comp] =
                        data.gradients_quad[(out_comp * n_q_points + point) *
                                              dim +
                                            j][in_comp];
                    }
          }
        if (update_flags & update_covariant_transformation)
          if (cell_similarity != CellSimilarity::translation)
            for (unsigned int point = 0; point < n_q_points; ++point)
              data.covariant[point] =
                (data.contravariant[point]).covariant_form();

        if (update_flags & update_volume_elements)
          if (cell_similarity != CellSimilarity::translation)
            for (unsigned int point = 0; point < n_q_points; ++point)
              data.volume_elements[point] =
                data.contravariant[point].determinant();

        if (evaluation_flag & EvaluationFlags::hessians)
          {
            constexpr int desymmetrize_3d[6][2] = {
              {0, 0}, {1, 1}, {2, 2}, {0, 1}, {0, 2}, {1, 2}};
            constexpr int desymmetrize_2d[3][2] = {{0, 0}, {1, 1}, {0, 1}};

            // We need to reinterpret the data after evaluate has been applied.
            for (unsigned int out_comp = 0; out_comp < n_comp; ++out_comp)
              for (unsigned int point = 0; point < n_q_points; ++point)
                for (unsigned int j = 0; j < n_hessians; ++j)
                  for (unsigned int in_comp = 0;
                       in_comp < n_lanes &&
                       in_comp < spacedim - out_comp * n_lanes;
                       ++in_comp)
                    {
                      const unsigned int total_number = point * n_hessians + j;
                      const unsigned int new_point = total_number % n_q_points;
                      const unsigned int new_hessian_comp =
                        total_number / n_q_points;
                      const unsigned int new_hessian_comp_i =
                        dim == 2 ? desymmetrize_2d[new_hessian_comp][0] :
                                   desymmetrize_3d[new_hessian_comp][0];
                      const unsigned int new_hessian_comp_j =
                        dim == 2 ? desymmetrize_2d[new_hessian_comp][1] :
                                   desymmetrize_3d[new_hessian_comp][1];
                      const double value =
                        data.hessians_quad[(out_comp * n_q_points + point) *
                                             n_hessians +
                                           j][in_comp];
                      jacobian_grads[new_point][out_comp * n_lanes + in_comp]
                                    [new_hessian_comp_i][new_hessian_comp_j] =
                                      value;
                      jacobian_grads[new_point][out_comp * n_lanes + in_comp]
                                    [new_hessian_comp_j][new_hessian_comp_i] =
                                      value;
                    }
          }
      }


      /**
       * Compute the locations of quadrature points on the object described by
       * the first argument (and the cell for which the mapping support points
       * have already been set), but only if the update_flags of the @p data
       * argument indicate so.
       */
      template <int dim, int spacedim>
      void
      maybe_compute_q_points(
        const typename QProjector<dim>::DataSetDescriptor data_set,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &                           data,
        std::vector<Point<spacedim>> &quadrature_points)
      {
        const UpdateFlags update_flags = data.update_each;

        if (update_flags & update_quadrature_points)
          for (unsigned int point = 0; point < quadrature_points.size();
               ++point)
            {
              const double *  shape = &data.shape(point + data_set, 0);
              Point<spacedim> result =
                (shape[0] * data.mapping_support_points[0]);
              for (unsigned int k = 1; k < data.n_shape_functions; ++k)
                for (unsigned int i = 0; i < spacedim; ++i)
                  result[i] += shape[k] * data.mapping_support_points[k][i];
              quadrature_points[point] = result;
            }
      }



      /**
       * Update the co- and contravariant matrices as well as their determinant,
       * for the cell
       * described stored in the data object, but only if the update_flags of the @p data
       * argument indicate so.
       *
       * Skip the computation if possible as indicated by the first argument.
       */
      template <int dim, int spacedim>
      void
      maybe_update_Jacobians(
        const CellSimilarity::Similarity cell_similarity,
        const typename dealii::QProjector<dim>::DataSetDescriptor data_set,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &data)
      {
        const UpdateFlags update_flags = data.update_each;

        if (update_flags & update_contravariant_transformation)
          // if the current cell is just a
          // translation of the previous one, no
          // need to recompute jacobians...
          if (cell_similarity != CellSimilarity::translation)
            {
              const unsigned int n_q_points = data.contravariant.size();

              std::fill(data.contravariant.begin(),
                        data.contravariant.end(),
                        DerivativeForm<1, dim, spacedim>());

              Assert(data.n_shape_functions > 0, ExcInternalError());

              const Tensor<1, spacedim> *supp_pts =
                data.mapping_support_points.data();

              for (unsigned int point = 0; point < n_q_points; ++point)
                {
                  const Tensor<1, dim> *data_derv =
                    &data.derivative(point + data_set, 0);

                  double result[spacedim][dim];

                  // peel away part of sum to avoid zeroing the
                  // entries and adding for the first time
                  for (unsigned int i = 0; i < spacedim; ++i)
                    for (unsigned int j = 0; j < dim; ++j)
                      result[i][j] = data_derv[0][j] * supp_pts[0][i];
                  for (unsigned int k = 1; k < data.n_shape_functions; ++k)
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < dim; ++j)
                        result[i][j] += data_derv[k][j] * supp_pts[k][i];

                  // write result into contravariant data. for
                  // j=dim in the case dim<spacedim, there will
                  // never be any nonzero data that arrives in
                  // here, so it is ok anyway because it was
                  // initialized to zero at the initialization
                  for (unsigned int i = 0; i < spacedim; ++i)
                    for (unsigned int j = 0; j < dim; ++j)
                      data.contravariant[point][i][j] = result[i][j];
                }
            }

        if (update_flags & update_covariant_transformation)
          if (cell_similarity != CellSimilarity::translation)
            {
              const unsigned int n_q_points = data.contravariant.size();
              for (unsigned int point = 0; point < n_q_points; ++point)
                {
                  data.covariant[point] =
                    (data.contravariant[point]).covariant_form();
                }
            }

        if (update_flags & update_volume_elements)
          if (cell_similarity != CellSimilarity::translation)
            {
              const unsigned int n_q_points = data.contravariant.size();
              for (unsigned int point = 0; point < n_q_points; ++point)
                data.volume_elements[point] =
                  data.contravariant[point].determinant();
            }
      }

      /**
       * Update the Hessian of the transformation from unit to real cell, the
       * Jacobian gradients.
       *
       * Skip the computation if possible as indicated by the first argument.
       */
      template <int dim, int spacedim>
      void
      maybe_update_jacobian_grads(
        const CellSimilarity::Similarity                  cell_similarity,
        const typename QProjector<dim>::DataSetDescriptor data_set,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &                                            data,
        std::vector<DerivativeForm<2, dim, spacedim>> &jacobian_grads)
      {
        const UpdateFlags update_flags = data.update_each;
        if (update_flags & update_jacobian_grads)
          {
            const unsigned int n_q_points = jacobian_grads.size();

            if (cell_similarity != CellSimilarity::translation)
              for (unsigned int point = 0; point < n_q_points; ++point)
                {
                  const Tensor<2, dim> *second =
                    &data.second_derivative(point + data_set, 0);
                  double result[spacedim][dim][dim];
                  for (unsigned int i = 0; i < spacedim; ++i)
                    for (unsigned int j = 0; j < dim; ++j)
                      for (unsigned int l = 0; l < dim; ++l)
                        result[i][j][l] =
                          (second[0][j][l] * data.mapping_support_points[0][i]);
                  for (unsigned int k = 1; k < data.n_shape_functions; ++k)
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < dim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          result[i][j][l] +=
                            (second[k][j][l] *
                             data.mapping_support_points[k][i]);

                  for (unsigned int i = 0; i < spacedim; ++i)
                    for (unsigned int j = 0; j < dim; ++j)
                      for (unsigned int l = 0; l < dim; ++l)
                        jacobian_grads[point][i][j][l] = result[i][j][l];
                }
          }
      }

      /**
       * Update the Hessian of the transformation from unit to real cell, the
       * Jacobian gradients, pushed forward to the real cell coordinates.
       *
       * Skip the computation if possible as indicated by the first argument.
       */
      template <int dim, int spacedim>
      void
      maybe_update_jacobian_pushed_forward_grads(
        const CellSimilarity::Similarity                  cell_similarity,
        const typename QProjector<dim>::DataSetDescriptor data_set,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &                               data,
        std::vector<Tensor<3, spacedim>> &jacobian_pushed_forward_grads)
      {
        const UpdateFlags update_flags = data.update_each;
        if (update_flags & update_jacobian_pushed_forward_grads)
          {
            const unsigned int n_q_points =
              jacobian_pushed_forward_grads.size();

            if (cell_similarity != CellSimilarity::translation)
              {
                double tmp[spacedim][spacedim][spacedim];
                for (unsigned int point = 0; point < n_q_points; ++point)
                  {
                    const Tensor<2, dim> *second =
                      &data.second_derivative(point + data_set, 0);
                    double result[spacedim][dim][dim];
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < dim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          result[i][j][l] = (second[0][j][l] *
                                             data.mapping_support_points[0][i]);
                    for (unsigned int k = 1; k < data.n_shape_functions; ++k)
                      for (unsigned int i = 0; i < spacedim; ++i)
                        for (unsigned int j = 0; j < dim; ++j)
                          for (unsigned int l = 0; l < dim; ++l)
                            result[i][j][l] +=
                              (second[k][j][l] *
                               data.mapping_support_points[k][i]);

                    // first push forward the j-components
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < spacedim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          {
                            tmp[i][j][l] =
                              result[i][0][l] * data.covariant[point][j][0];
                            for (unsigned int jr = 1; jr < dim; ++jr)
                              {
                                tmp[i][j][l] += result[i][jr][l] *
                                                data.covariant[point][j][jr];
                              }
                          }

                    // now, pushing forward the l-components
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < spacedim; ++j)
                        for (unsigned int l = 0; l < spacedim; ++l)
                          {
                            jacobian_pushed_forward_grads[point][i][j][l] =
                              tmp[i][j][0] * data.covariant[point][l][0];
                            for (unsigned int lr = 1; lr < dim; ++lr)
                              {
                                jacobian_pushed_forward_grads[point][i][j][l] +=
                                  tmp[i][j][lr] * data.covariant[point][l][lr];
                              }
                          }
                  }
              }
          }
      }

      /**
       * Update the third derivatives of the transformation from unit to real
       * cell, the Jacobian hessians.
       *
       * Skip the computation if possible as indicated by the first argument.
       */
      template <int dim, int spacedim>
      void
      maybe_update_jacobian_2nd_derivatives(
        const CellSimilarity::Similarity                  cell_similarity,
        const typename QProjector<dim>::DataSetDescriptor data_set,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &                                            data,
        std::vector<DerivativeForm<3, dim, spacedim>> &jacobian_2nd_derivatives)
      {
        const UpdateFlags update_flags = data.update_each;
        if (update_flags & update_jacobian_2nd_derivatives)
          {
            const unsigned int n_q_points = jacobian_2nd_derivatives.size();

            if (cell_similarity != CellSimilarity::translation)
              {
                for (unsigned int point = 0; point < n_q_points; ++point)
                  {
                    const Tensor<3, dim> *third =
                      &data.third_derivative(point + data_set, 0);
                    double result[spacedim][dim][dim][dim];
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < dim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          for (unsigned int m = 0; m < dim; ++m)
                            result[i][j][l][m] =
                              (third[0][j][l][m] *
                               data.mapping_support_points[0][i]);
                    for (unsigned int k = 1; k < data.n_shape_functions; ++k)
                      for (unsigned int i = 0; i < spacedim; ++i)
                        for (unsigned int j = 0; j < dim; ++j)
                          for (unsigned int l = 0; l < dim; ++l)
                            for (unsigned int m = 0; m < dim; ++m)
                              result[i][j][l][m] +=
                                (third[k][j][l][m] *
                                 data.mapping_support_points[k][i]);

                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < dim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          for (unsigned int m = 0; m < dim; ++m)
                            jacobian_2nd_derivatives[point][i][j][l][m] =
                              result[i][j][l][m];
                  }
              }
          }
      }

      /**
       * Update the Hessian of the Hessian of the transformation from unit
       * to real cell, the Jacobian Hessian gradients, pushed forward to the
       * real cell coordinates.
       *
       * Skip the computation if possible as indicated by the first argument.
       */
      template <int dim, int spacedim>
      void
      maybe_update_jacobian_pushed_forward_2nd_derivatives(
        const CellSimilarity::Similarity                  cell_similarity,
        const typename QProjector<dim>::DataSetDescriptor data_set,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &data,
        std::vector<Tensor<4, spacedim>>
          &jacobian_pushed_forward_2nd_derivatives)
      {
        const UpdateFlags update_flags = data.update_each;
        if (update_flags & update_jacobian_pushed_forward_2nd_derivatives)
          {
            const unsigned int n_q_points =
              jacobian_pushed_forward_2nd_derivatives.size();

            if (cell_similarity != CellSimilarity::translation)
              {
                double tmp[spacedim][spacedim][spacedim][spacedim];
                for (unsigned int point = 0; point < n_q_points; ++point)
                  {
                    const Tensor<3, dim> *third =
                      &data.third_derivative(point + data_set, 0);
                    double result[spacedim][dim][dim][dim];
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < dim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          for (unsigned int m = 0; m < dim; ++m)
                            result[i][j][l][m] =
                              (third[0][j][l][m] *
                               data.mapping_support_points[0][i]);
                    for (unsigned int k = 1; k < data.n_shape_functions; ++k)
                      for (unsigned int i = 0; i < spacedim; ++i)
                        for (unsigned int j = 0; j < dim; ++j)
                          for (unsigned int l = 0; l < dim; ++l)
                            for (unsigned int m = 0; m < dim; ++m)
                              result[i][j][l][m] +=
                                (third[k][j][l][m] *
                                 data.mapping_support_points[k][i]);

                    // push forward the j-coordinate
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < spacedim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          for (unsigned int m = 0; m < dim; ++m)
                            {
                              jacobian_pushed_forward_2nd_derivatives
                                [point][i][j][l][m] =
                                  result[i][0][l][m] *
                                  data.covariant[point][j][0];
                              for (unsigned int jr = 1; jr < dim; ++jr)
                                jacobian_pushed_forward_2nd_derivatives[point]
                                                                       [i][j][l]
                                                                       [m] +=
                                  result[i][jr][l][m] *
                                  data.covariant[point][j][jr];
                            }

                    // push forward the l-coordinate
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < spacedim; ++j)
                        for (unsigned int l = 0; l < spacedim; ++l)
                          for (unsigned int m = 0; m < dim; ++m)
                            {
                              tmp[i][j][l][m] =
                                jacobian_pushed_forward_2nd_derivatives[point]
                                                                       [i][j][0]
                                                                       [m] *
                                data.covariant[point][l][0];
                              for (unsigned int lr = 1; lr < dim; ++lr)
                                tmp[i][j][l][m] +=
                                  jacobian_pushed_forward_2nd_derivatives
                                    [point][i][j][lr][m] *
                                  data.covariant[point][l][lr];
                            }

                    // push forward the m-coordinate
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < spacedim; ++j)
                        for (unsigned int l = 0; l < spacedim; ++l)
                          for (unsigned int m = 0; m < spacedim; ++m)
                            {
                              jacobian_pushed_forward_2nd_derivatives
                                [point][i][j][l][m] =
                                  tmp[i][j][l][0] * data.covariant[point][m][0];
                              for (unsigned int mr = 1; mr < dim; ++mr)
                                jacobian_pushed_forward_2nd_derivatives[point]
                                                                       [i][j][l]
                                                                       [m] +=
                                  tmp[i][j][l][mr] *
                                  data.covariant[point][m][mr];
                            }
                  }
              }
          }
      }

      /**
       * Update the fourth derivatives of the transformation from unit to real
       * cell, the Jacobian hessian gradients.
       *
       * Skip the computation if possible as indicated by the first argument.
       */
      template <int dim, int spacedim>
      void
      maybe_update_jacobian_3rd_derivatives(
        const CellSimilarity::Similarity                  cell_similarity,
        const typename QProjector<dim>::DataSetDescriptor data_set,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &                                            data,
        std::vector<DerivativeForm<4, dim, spacedim>> &jacobian_3rd_derivatives)
      {
        const UpdateFlags update_flags = data.update_each;
        if (update_flags & update_jacobian_3rd_derivatives)
          {
            const unsigned int n_q_points = jacobian_3rd_derivatives.size();

            if (cell_similarity != CellSimilarity::translation)
              {
                for (unsigned int point = 0; point < n_q_points; ++point)
                  {
                    const Tensor<4, dim> *fourth =
                      &data.fourth_derivative(point + data_set, 0);
                    double result[spacedim][dim][dim][dim][dim];
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < dim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          for (unsigned int m = 0; m < dim; ++m)
                            for (unsigned int n = 0; n < dim; ++n)
                              result[i][j][l][m][n] =
                                (fourth[0][j][l][m][n] *
                                 data.mapping_support_points[0][i]);
                    for (unsigned int k = 1; k < data.n_shape_functions; ++k)
                      for (unsigned int i = 0; i < spacedim; ++i)
                        for (unsigned int j = 0; j < dim; ++j)
                          for (unsigned int l = 0; l < dim; ++l)
                            for (unsigned int m = 0; m < dim; ++m)
                              for (unsigned int n = 0; n < dim; ++n)
                                result[i][j][l][m][n] +=
                                  (fourth[k][j][l][m][n] *
                                   data.mapping_support_points[k][i]);

                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < dim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          for (unsigned int m = 0; m < dim; ++m)
                            for (unsigned int n = 0; n < dim; ++n)
                              jacobian_3rd_derivatives[point][i][j][l][m][n] =
                                result[i][j][l][m][n];
                  }
              }
          }
      }

      /**
       * Update the Hessian gradient of the transformation from unit to real
       * cell, the Jacobian Hessians, pushed forward to the real cell
       * coordinates.
       *
       * Skip the computation if possible as indicated by the first argument.
       */
      template <int dim, int spacedim>
      void
      maybe_update_jacobian_pushed_forward_3rd_derivatives(
        const CellSimilarity::Similarity                  cell_similarity,
        const typename QProjector<dim>::DataSetDescriptor data_set,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &data,
        std::vector<Tensor<5, spacedim>>
          &jacobian_pushed_forward_3rd_derivatives)
      {
        const UpdateFlags update_flags = data.update_each;
        if (update_flags & update_jacobian_pushed_forward_3rd_derivatives)
          {
            const unsigned int n_q_points =
              jacobian_pushed_forward_3rd_derivatives.size();

            if (cell_similarity != CellSimilarity::translation)
              {
                double tmp[spacedim][spacedim][spacedim][spacedim][spacedim];
                for (unsigned int point = 0; point < n_q_points; ++point)
                  {
                    const Tensor<4, dim> *fourth =
                      &data.fourth_derivative(point + data_set, 0);
                    double result[spacedim][dim][dim][dim][dim];
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < dim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          for (unsigned int m = 0; m < dim; ++m)
                            for (unsigned int n = 0; n < dim; ++n)
                              result[i][j][l][m][n] =
                                (fourth[0][j][l][m][n] *
                                 data.mapping_support_points[0][i]);
                    for (unsigned int k = 1; k < data.n_shape_functions; ++k)
                      for (unsigned int i = 0; i < spacedim; ++i)
                        for (unsigned int j = 0; j < dim; ++j)
                          for (unsigned int l = 0; l < dim; ++l)
                            for (unsigned int m = 0; m < dim; ++m)
                              for (unsigned int n = 0; n < dim; ++n)
                                result[i][j][l][m][n] +=
                                  (fourth[k][j][l][m][n] *
                                   data.mapping_support_points[k][i]);

                    // push-forward the j-coordinate
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < spacedim; ++j)
                        for (unsigned int l = 0; l < dim; ++l)
                          for (unsigned int m = 0; m < dim; ++m)
                            for (unsigned int n = 0; n < dim; ++n)
                              {
                                tmp[i][j][l][m][n] =
                                  result[i][0][l][m][n] *
                                  data.covariant[point][j][0];
                                for (unsigned int jr = 1; jr < dim; ++jr)
                                  tmp[i][j][l][m][n] +=
                                    result[i][jr][l][m][n] *
                                    data.covariant[point][j][jr];
                              }

                    // push-forward the l-coordinate
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < spacedim; ++j)
                        for (unsigned int l = 0; l < spacedim; ++l)
                          for (unsigned int m = 0; m < dim; ++m)
                            for (unsigned int n = 0; n < dim; ++n)
                              {
                                jacobian_pushed_forward_3rd_derivatives
                                  [point][i][j][l][m][n] =
                                    tmp[i][j][0][m][n] *
                                    data.covariant[point][l][0];
                                for (unsigned int lr = 1; lr < dim; ++lr)
                                  jacobian_pushed_forward_3rd_derivatives
                                    [point][i][j][l][m][n] +=
                                    tmp[i][j][lr][m][n] *
                                    data.covariant[point][l][lr];
                              }

                    // push-forward the m-coordinate
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < spacedim; ++j)
                        for (unsigned int l = 0; l < spacedim; ++l)
                          for (unsigned int m = 0; m < spacedim; ++m)
                            for (unsigned int n = 0; n < dim; ++n)
                              {
                                tmp[i][j][l][m][n] =
                                  jacobian_pushed_forward_3rd_derivatives
                                    [point][i][j][l][0][n] *
                                  data.covariant[point][m][0];
                                for (unsigned int mr = 1; mr < dim; ++mr)
                                  tmp[i][j][l][m][n] +=
                                    jacobian_pushed_forward_3rd_derivatives
                                      [point][i][j][l][mr][n] *
                                    data.covariant[point][m][mr];
                              }

                    // push-forward the n-coordinate
                    for (unsigned int i = 0; i < spacedim; ++i)
                      for (unsigned int j = 0; j < spacedim; ++j)
                        for (unsigned int l = 0; l < spacedim; ++l)
                          for (unsigned int m = 0; m < spacedim; ++m)
                            for (unsigned int n = 0; n < spacedim; ++n)
                              {
                                jacobian_pushed_forward_3rd_derivatives
                                  [point][i][j][l][m][n] =
                                    tmp[i][j][l][m][0] *
                                    data.covariant[point][n][0];
                                for (unsigned int nr = 1; nr < dim; ++nr)
                                  jacobian_pushed_forward_3rd_derivatives
                                    [point][i][j][l][m][n] +=
                                    tmp[i][j][l][m][nr] *
                                    data.covariant[point][n][nr];
                              }
                  }
              }
          }
      }
    } // namespace
  }   // namespace MappingQGenericImplementation
} // namespace internal



template <int dim, int spacedim>
MappingQGeneric<dim, spacedim>::MappingQGeneric(const unsigned int p)
  : polynomial_degree(p)
  , line_support_points(
      QGaussLobatto<1>(this->polynomial_degree + 1).get_points())
  , polynomials_1d(
      Polynomials::generate_complete_Lagrange_basis(line_support_points))
  , renumber_lexicographic_to_hierarchic(
      FETools::lexicographic_to_hierarchic_numbering<dim>(p))
  , support_point_weights_perimeter_to_interior(
      internal::MappingQGenericImplementation::
        compute_support_point_weights_perimeter_to_interior(
          this->polynomial_degree,
          dim))
  , support_point_weights_cell(
      internal::MappingQGenericImplementation::
        compute_support_point_weights_cell<dim>(this->polynomial_degree))
{
  Assert(p >= 1,
         ExcMessage("It only makes sense to create polynomial mappings "
                    "with a polynomial degree greater or equal to one."));
}



template <int dim, int spacedim>
MappingQGeneric<dim, spacedim>::MappingQGeneric(
  const MappingQGeneric<dim, spacedim> &mapping)
  : polynomial_degree(mapping.polynomial_degree)
  , line_support_points(mapping.line_support_points)
  , polynomials_1d(mapping.polynomials_1d)
  , renumber_lexicographic_to_hierarchic(
      mapping.renumber_lexicographic_to_hierarchic)
  , support_point_weights_perimeter_to_interior(
      mapping.support_point_weights_perimeter_to_interior)
  , support_point_weights_cell(mapping.support_point_weights_cell)
{}



template <int dim, int spacedim>
std::unique_ptr<Mapping<dim, spacedim>>
MappingQGeneric<dim, spacedim>::clone() const
{
  return std::make_unique<MappingQGeneric<dim, spacedim>>(*this);
}



template <int dim, int spacedim>
unsigned int
MappingQGeneric<dim, spacedim>::get_degree() const
{
  return polynomial_degree;
}



template <int dim, int spacedim>
Point<spacedim>
MappingQGeneric<dim, spacedim>::transform_unit_to_real_cell(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const Point<dim> &                                          p) const
{
  return internal::MappingQGenericImplementation::
    compute_mapped_location_of_point(this->compute_mapping_support_points(cell),
                                     polynomials_1d,
                                     renumber_lexicographic_to_hierarchic,
                                     p)
      .first;
}


// In the code below, GCC tries to instantiate MappingQGeneric<3,4> when
// seeing which of the overloaded versions of
// do_transform_real_to_unit_cell_internal() to call. This leads to bad
// error messages and, generally, nothing very good. Avoid this by ensuring
// that this class exists, but does not have an inner InternalData
// type, thereby ruling out the codim-1 version of the function
// below when doing overload resolution.
template <>
class MappingQGeneric<3, 4>
{};



// visual studio freaks out when trying to determine if
// do_transform_real_to_unit_cell_internal with dim=3 and spacedim=4 is a good
// candidate. So instead of letting the compiler pick the correct overload, we
// use template specialization to make sure we pick up the right function to
// call:

template <int dim, int spacedim>
Point<dim>
MappingQGeneric<dim, spacedim>::transform_real_to_unit_cell_internal(
  const typename Triangulation<dim, spacedim>::cell_iterator &,
  const Point<spacedim> &,
  const Point<dim> &) const
{
  // default implementation (should never be called)
  Assert(false, ExcInternalError());
  return Point<dim>();
}



template <>
Point<1>
MappingQGeneric<1, 1>::transform_real_to_unit_cell_internal(
  const Triangulation<1, 1>::cell_iterator &cell,
  const Point<1> &                          p,
  const Point<1> &                          initial_p_unit) const
{
  // dispatch to the various specializations for spacedim=dim,
  // spacedim=dim+1, etc
  return internal::MappingQGenericImplementation::
    do_transform_real_to_unit_cell_internal<1>(
      p,
      initial_p_unit,
      this->compute_mapping_support_points(cell),
      polynomials_1d,
      renumber_lexicographic_to_hierarchic);
}



template <>
Point<2>
MappingQGeneric<2, 2>::transform_real_to_unit_cell_internal(
  const Triangulation<2, 2>::cell_iterator &cell,
  const Point<2> &                          p,
  const Point<2> &                          initial_p_unit) const
{
  return internal::MappingQGenericImplementation::
    do_transform_real_to_unit_cell_internal<2>(
      p,
      initial_p_unit,
      this->compute_mapping_support_points(cell),
      polynomials_1d,
      renumber_lexicographic_to_hierarchic);
}



template <>
Point<3>
MappingQGeneric<3, 3>::transform_real_to_unit_cell_internal(
  const Triangulation<3, 3>::cell_iterator &cell,
  const Point<3> &                          p,
  const Point<3> &                          initial_p_unit) const
{
  return internal::MappingQGenericImplementation::
    do_transform_real_to_unit_cell_internal<3>(
      p,
      initial_p_unit,
      this->compute_mapping_support_points(cell),
      polynomials_1d,
      renumber_lexicographic_to_hierarchic);
}



template <>
Point<1>
MappingQGeneric<1, 2>::transform_real_to_unit_cell_internal(
  const Triangulation<1, 2>::cell_iterator &cell,
  const Point<2> &                          p,
  const Point<1> &                          initial_p_unit) const
{
  const int dim      = 1;
  const int spacedim = 2;

  const Quadrature<dim> point_quadrature(initial_p_unit);

  UpdateFlags update_flags = update_quadrature_points | update_jacobians;
  if (spacedim > dim)
    update_flags |= update_jacobian_grads;
  auto mdata = Utilities::dynamic_unique_cast<InternalData>(
    get_data(update_flags, point_quadrature));

  mdata->mapping_support_points = this->compute_mapping_support_points(cell);

  // dispatch to the various specializations for spacedim=dim,
  // spacedim=dim+1, etc
  return internal::MappingQGenericImplementation::
    do_transform_real_to_unit_cell_internal_codim1<1>(cell,
                                                      p,
                                                      initial_p_unit,
                                                      *mdata);
}



template <>
Point<2>
MappingQGeneric<2, 3>::transform_real_to_unit_cell_internal(
  const Triangulation<2, 3>::cell_iterator &cell,
  const Point<3> &                          p,
  const Point<2> &                          initial_p_unit) const
{
  const int dim      = 2;
  const int spacedim = 3;

  const Quadrature<dim> point_quadrature(initial_p_unit);

  UpdateFlags update_flags = update_quadrature_points | update_jacobians;
  if (spacedim > dim)
    update_flags |= update_jacobian_grads;
  auto mdata = Utilities::dynamic_unique_cast<InternalData>(
    get_data(update_flags, point_quadrature));

  mdata->mapping_support_points = this->compute_mapping_support_points(cell);

  // dispatch to the various specializations for spacedim=dim,
  // spacedim=dim+1, etc
  return internal::MappingQGenericImplementation::
    do_transform_real_to_unit_cell_internal_codim1<2>(cell,
                                                      p,
                                                      initial_p_unit,
                                                      *mdata);
}

template <>
Point<1>
MappingQGeneric<1, 3>::transform_real_to_unit_cell_internal(
  const Triangulation<1, 3>::cell_iterator &,
  const Point<3> &,
  const Point<1> &) const
{
  Assert(false, ExcNotImplemented());
  return {};
}



template <int dim, int spacedim>
Point<dim>
MappingQGeneric<dim, spacedim>::transform_real_to_unit_cell(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const Point<spacedim> &                                     p) const
{
  // Use an exact formula if one is available. this is only the case
  // for Q1 mappings in 1d, and in 2d if dim==spacedim
  if (this->preserves_vertex_locations() && (polynomial_degree == 1) &&
      ((dim == 1) || ((dim == 2) && (dim == spacedim))))
    {
      // The dimension-dependent algorithms are much faster (about 25-45x in
      // 2D) but fail most of the time when the given point (p) is not in the
      // cell. The dimension-independent Newton algorithm given below is
      // slower, but more robust (though it still sometimes fails). Therefore
      // this function implements the following strategy based on the
      // p's dimension:
      //
      // * In 1D this mapping is linear, so the mapping is always invertible
      //   (and the exact formula is known) as long as the cell has non-zero
      //   length.
      // * In 2D the exact (quadratic) formula is called first. If either the
      //   exact formula does not succeed (negative discriminant in the
      //   quadratic formula) or succeeds but finds a solution outside of the
      //   unit cell, then the Newton solver is called. The rationale for the
      //   second choice is that the exact formula may provide two different
      //   answers when mapping a point outside of the real cell, but the
      //   Newton solver (if it converges) will only return one answer.
      //   Otherwise the exact formula successfully found a point in the unit
      //   cell and that value is returned.
      // * In 3D there is no (known to the authors) exact formula, so the Newton
      //   algorithm is used.
      const std::array<Point<spacedim>, GeometryInfo<dim>::vertices_per_cell>
        vertices = this->get_vertices(cell);
      try
        {
          switch (dim)
            {
              case 1:
                {
                  // formula not subject to any issues in 1d
                  if (spacedim == 1)
                    return internal::MappingQ1::transform_real_to_unit_cell(
                      vertices, p);
                  else
                    break;
                }

              case 2:
                {
                  const Point<dim> point =
                    internal::MappingQ1::transform_real_to_unit_cell(vertices,
                                                                     p);

                  // formula not guaranteed to work for points outside of
                  // the cell. only take the computed point if it lies
                  // inside the reference cell
                  const double eps = 1e-15;
                  if (-eps <= point(1) && point(1) <= 1 + eps &&
                      -eps <= point(0) && point(0) <= 1 + eps)
                    {
                      return point;
                    }
                  else
                    break;
                }

              default:
                {
                  // we should get here, based on the if-condition at the top
                  Assert(false, ExcInternalError());
                }
            }
        }
      catch (
        const typename Mapping<spacedim, spacedim>::ExcTransformationFailed &)
        {
          // simply fall through and continue on to the standard Newton code
        }
    }
  else
    {
      // we can't use an explicit formula,
    }


  // Find the initial value for the Newton iteration by a normal
  // projection to the least square plane determined by the vertices
  // of the cell
  Point<dim> initial_p_unit;
  if (this->preserves_vertex_locations())
    {
      initial_p_unit = cell->real_to_unit_cell_affine_approximation(p);
      // in 1d with spacedim > 1 the affine approximation is exact
      if (dim == 1 && polynomial_degree == 1)
        return initial_p_unit;
    }
  else
    {
      // else, we simply use the mid point
      for (unsigned int d = 0; d < dim; ++d)
        initial_p_unit[d] = 0.5;
    }

  // in case the function above should have given us something back that lies
  // outside the unit cell, then project it back into the reference cell in
  // hopes that this gives a better starting point to the following iteration
  initial_p_unit = GeometryInfo<dim>::project_to_unit_cell(initial_p_unit);

  // perform the Newton iteration and return the result. note that this
  // statement may throw an exception, which we simply pass up to the caller
  const Point<dim> p_unit =
    this->transform_real_to_unit_cell_internal(cell, p, initial_p_unit);
  if (p_unit[0] == std::numeric_limits<double>::infinity())
    AssertThrow(false,
                (typename Mapping<dim, spacedim>::ExcTransformationFailed()));
  return p_unit;
}



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::transform_points_real_to_unit_cell(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const ArrayView<const Point<spacedim>> &                    real_points,
  ArrayView<Point<dim>> &                                     unit_points) const
{
  AssertDimension(real_points.size(), unit_points.size());
  const std::vector<Point<spacedim>> support_points =
    this->compute_mapping_support_points(cell);

  // From the chosen (high-order) support points, now only pick the first
  // 2^dim points and construct an affine approximation from those.
  const std::pair<DerivativeForm<1, dim, spacedim>, Tensor<1, spacedim>>
    affine_factors = GridTools::affine_cell_approximation<dim>(
      ArrayView<const Point<spacedim>>(support_points.data(),
                                       GeometryInfo<dim>::vertices_per_cell));
  const DerivativeForm<1, spacedim, dim> A_inv =
    affine_factors.first.covariant_form().transpose();

  for (unsigned int i = 0; i < real_points.size(); ++i)
    {
      try
        {
          // Compute an initial guess by inverting the affine approximation
          // A * x_unit + b = x_real
          Point<dim> initial_guess(
            apply_transformation(A_inv,
                                 real_points[i] - affine_factors.second));
          unit_points[i] = internal::MappingQGenericImplementation::
            do_transform_real_to_unit_cell_internal<dim, spacedim>(
              real_points[i],
              GeometryInfo<dim>::project_to_unit_cell(initial_guess),
              support_points,
              polynomials_1d,
              renumber_lexicographic_to_hierarchic);
        }
      catch (typename Mapping<dim>::ExcTransformationFailed &)
        {
          unit_points[i][0] = std::numeric_limits<double>::infinity();
        }
    }
}



template <int dim, int spacedim>
UpdateFlags
MappingQGeneric<dim, spacedim>::requires_update_flags(
  const UpdateFlags in) const
{
  // add flags if the respective quantities are necessary to compute
  // what we need. note that some flags appear in both the conditions
  // and in subsequent set operations. this leads to some circular
  // logic. the only way to treat this is to iterate. since there are
  // 5 if-clauses in the loop, it will take at most 5 iterations to
  // converge. do them:
  UpdateFlags out = in;
  for (unsigned int i = 0; i < 5; ++i)
    {
      // The following is a little incorrect:
      // If not applied on a face,
      // update_boundary_forms does not
      // make sense. On the other hand,
      // it is necessary on a
      // face. Currently,
      // update_boundary_forms is simply
      // ignored for the interior of a
      // cell.
      if (out & (update_JxW_values | update_normal_vectors))
        out |= update_boundary_forms;

      if (out & (update_covariant_transformation | update_JxW_values |
                 update_jacobians | update_jacobian_grads |
                 update_boundary_forms | update_normal_vectors))
        out |= update_contravariant_transformation;

      if (out &
          (update_inverse_jacobians | update_jacobian_pushed_forward_grads |
           update_jacobian_pushed_forward_2nd_derivatives |
           update_jacobian_pushed_forward_3rd_derivatives))
        out |= update_covariant_transformation;

      // The contravariant transformation is used in the Piola
      // transformation, which requires the determinant of the Jacobi
      // matrix of the transformation.  Because we have no way of
      // knowing here whether the finite element wants to use the
      // contravariant or the Piola transforms, we add the JxW values
      // to the list of flags to be updated for each cell.
      if (out & update_contravariant_transformation)
        out |= update_volume_elements;

      // the same is true when computing normal vectors: they require
      // the determinant of the Jacobian
      if (out & update_normal_vectors)
        out |= update_volume_elements;
    }

  return out;
}



template <int dim, int spacedim>
std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase>
MappingQGeneric<dim, spacedim>::get_data(const UpdateFlags      update_flags,
                                         const Quadrature<dim> &q) const
{
  std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase> data_ptr =
    std::make_unique<InternalData>(polynomial_degree);
  auto &data = dynamic_cast<InternalData &>(*data_ptr);
  data.initialize(this->requires_update_flags(update_flags), q, q.size());

  return data_ptr;
}



template <int dim, int spacedim>
std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase>
MappingQGeneric<dim, spacedim>::get_face_data(
  const UpdateFlags          update_flags,
  const Quadrature<dim - 1> &quadrature) const
{
  std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase> data_ptr =
    std::make_unique<InternalData>(polynomial_degree);
  auto &data = dynamic_cast<InternalData &>(*data_ptr);
  data.initialize_face(this->requires_update_flags(update_flags),
                       QProjector<dim>::project_to_all_faces(
                         ReferenceCell::get_hypercube(dim), quadrature),
                       quadrature.size());

  return data_ptr;
}



template <int dim, int spacedim>
std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase>
MappingQGeneric<dim, spacedim>::get_subface_data(
  const UpdateFlags          update_flags,
  const Quadrature<dim - 1> &quadrature) const
{
  std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase> data_ptr =
    std::make_unique<InternalData>(polynomial_degree);
  auto &data = dynamic_cast<InternalData &>(*data_ptr);
  data.initialize_face(this->requires_update_flags(update_flags),
                       QProjector<dim>::project_to_all_subfaces(
                         ReferenceCell::get_hypercube(dim), quadrature),
                       quadrature.size());

  return data_ptr;
}



template <int dim, int spacedim>
CellSimilarity::Similarity
MappingQGeneric<dim, spacedim>::fill_fe_values(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const CellSimilarity::Similarity                            cell_similarity,
  const Quadrature<dim> &                                     quadrature,
  const typename Mapping<dim, spacedim>::InternalDataBase &   internal_data,
  internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
    &output_data) const
{
  // ensure that the following static_cast is really correct:
  Assert(dynamic_cast<const InternalData *>(&internal_data) != nullptr,
         ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(internal_data);

  const unsigned int n_q_points = quadrature.size();

  // recompute the support points of the transformation of this
  // cell. we tried to be clever here in an earlier version of the
  // library by checking whether the cell is the same as the one we
  // had visited last, but it turns out to be difficult to determine
  // that because a cell for the purposes of a mapping is
  // characterized not just by its (triangulation, level, index)
  // triple, but also by the locations of its vertices, the manifold
  // object attached to the cell and all of its bounding faces/edges,
  // etc. to reliably test that the "cell" we are on is, therefore,
  // not easily done
  data.mapping_support_points = this->compute_mapping_support_points(cell);
  data.cell_of_current_support_points = cell;

  // if the order of the mapping is greater than 1, then do not reuse any cell
  // similarity information. This is necessary because the cell similarity
  // value is computed with just cell vertices and does not take into account
  // cell curvature.
  const CellSimilarity::Similarity computed_cell_similarity =
    (polynomial_degree == 1 ? cell_similarity : CellSimilarity::none);

  if (dim > 1 && data.tensor_product_quadrature)
    {
      internal::MappingQGenericImplementation::
        maybe_update_q_points_Jacobians_and_grads_tensor<dim, spacedim>(
          computed_cell_similarity,
          data,
          output_data.quadrature_points,
          output_data.jacobian_grads);
    }
  else
    {
      internal::MappingQGenericImplementation::maybe_compute_q_points<dim,
                                                                      spacedim>(
        QProjector<dim>::DataSetDescriptor::cell(),
        data,
        output_data.quadrature_points);

      internal::MappingQGenericImplementation::maybe_update_Jacobians<dim,
                                                                      spacedim>(
        computed_cell_similarity,
        QProjector<dim>::DataSetDescriptor::cell(),
        data);

      internal::MappingQGenericImplementation::maybe_update_jacobian_grads<
        dim,
        spacedim>(computed_cell_similarity,
                  QProjector<dim>::DataSetDescriptor::cell(),
                  data,
                  output_data.jacobian_grads);
    }

  internal::MappingQGenericImplementation::
    maybe_update_jacobian_pushed_forward_grads<dim, spacedim>(
      computed_cell_similarity,
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      output_data.jacobian_pushed_forward_grads);

  internal::MappingQGenericImplementation::
    maybe_update_jacobian_2nd_derivatives<dim, spacedim>(
      computed_cell_similarity,
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      output_data.jacobian_2nd_derivatives);

  internal::MappingQGenericImplementation::
    maybe_update_jacobian_pushed_forward_2nd_derivatives<dim, spacedim>(
      computed_cell_similarity,
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      output_data.jacobian_pushed_forward_2nd_derivatives);

  internal::MappingQGenericImplementation::
    maybe_update_jacobian_3rd_derivatives<dim, spacedim>(
      computed_cell_similarity,
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      output_data.jacobian_3rd_derivatives);

  internal::MappingQGenericImplementation::
    maybe_update_jacobian_pushed_forward_3rd_derivatives<dim, spacedim>(
      computed_cell_similarity,
      QProjector<dim>::DataSetDescriptor::cell(),
      data,
      output_data.jacobian_pushed_forward_3rd_derivatives);

  const UpdateFlags          update_flags = data.update_each;
  const std::vector<double> &weights      = quadrature.get_weights();

  // Multiply quadrature weights by absolute value of Jacobian determinants or
  // the area element g=sqrt(DX^t DX) in case of codim > 0

  if (update_flags & (update_normal_vectors | update_JxW_values))
    {
      AssertDimension(output_data.JxW_values.size(), n_q_points);

      Assert(!(update_flags & update_normal_vectors) ||
               (output_data.normal_vectors.size() == n_q_points),
             ExcDimensionMismatch(output_data.normal_vectors.size(),
                                  n_q_points));


      if (computed_cell_similarity != CellSimilarity::translation)
        for (unsigned int point = 0; point < n_q_points; ++point)
          {
            if (dim == spacedim)
              {
                const double det = data.contravariant[point].determinant();

                // check for distorted cells.

                // TODO: this allows for anisotropies of up to 1e6 in 3D and
                // 1e12 in 2D. might want to find a finer
                // (dimension-independent) criterion
                Assert(det >
                         1e-12 * Utilities::fixed_power<dim>(
                                   cell->diameter() / std::sqrt(double(dim))),
                       (typename Mapping<dim, spacedim>::ExcDistortedMappedCell(
                         cell->center(), det, point)));

                output_data.JxW_values[point] = weights[point] * det;
              }
            // if dim==spacedim, then there is no cell normal to
            // compute. since this is for FEValues (and not FEFaceValues),
            // there are also no face normals to compute
            else // codim>0 case
              {
                Tensor<1, spacedim> DX_t[dim];
                for (unsigned int i = 0; i < spacedim; ++i)
                  for (unsigned int j = 0; j < dim; ++j)
                    DX_t[j][i] = data.contravariant[point][i][j];

                Tensor<2, dim> G; // First fundamental form
                for (unsigned int i = 0; i < dim; ++i)
                  for (unsigned int j = 0; j < dim; ++j)
                    G[i][j] = DX_t[i] * DX_t[j];

                output_data.JxW_values[point] =
                  std::sqrt(determinant(G)) * weights[point];

                if (computed_cell_similarity ==
                    CellSimilarity::inverted_translation)
                  {
                    // we only need to flip the normal
                    if (update_flags & update_normal_vectors)
                      output_data.normal_vectors[point] *= -1.;
                  }
                else
                  {
                    if (update_flags & update_normal_vectors)
                      {
                        Assert(spacedim == dim + 1,
                               ExcMessage(
                                 "There is no (unique) cell normal for " +
                                 Utilities::int_to_string(dim) +
                                 "-dimensional cells in " +
                                 Utilities::int_to_string(spacedim) +
                                 "-dimensional space. This only works if the "
                                 "space dimension is one greater than the "
                                 "dimensionality of the mesh cells."));

                        if (dim == 1)
                          output_data.normal_vectors[point] =
                            cross_product_2d(-DX_t[0]);
                        else // dim == 2
                          output_data.normal_vectors[point] =
                            cross_product_3d(DX_t[0], DX_t[1]);

                        output_data.normal_vectors[point] /=
                          output_data.normal_vectors[point].norm();

                        if (cell->direction_flag() == false)
                          output_data.normal_vectors[point] *= -1.;
                      }
                  }
              } // codim>0 case
          }
    }



  // copy values from InternalData to vector given by reference
  if (update_flags & update_jacobians)
    {
      AssertDimension(output_data.jacobians.size(), n_q_points);
      if (computed_cell_similarity != CellSimilarity::translation)
        for (unsigned int point = 0; point < n_q_points; ++point)
          output_data.jacobians[point] = data.contravariant[point];
    }

  // copy values from InternalData to vector given by reference
  if (update_flags & update_inverse_jacobians)
    {
      AssertDimension(output_data.inverse_jacobians.size(), n_q_points);
      if (computed_cell_similarity != CellSimilarity::translation)
        for (unsigned int point = 0; point < n_q_points; ++point)
          output_data.inverse_jacobians[point] =
            data.covariant[point].transpose();
    }

  return computed_cell_similarity;
}



namespace internal
{
  namespace MappingQGenericImplementation
  {
    namespace
    {
      /**
       * Depending on what information is called for in the update flags of the
       * @p data object, compute the various pieces of information that is
       * required by the fill_fe_face_values() and fill_fe_subface_values()
       * functions. This function simply unifies the work that would be done by
       * those two functions.
       *
       * The resulting data is put into the @p output_data argument.
       */
      template <int dim, int spacedim>
      void
      maybe_compute_face_data(
        const dealii::MappingQGeneric<dim, spacedim> &mapping,
        const typename dealii::Triangulation<dim, spacedim>::cell_iterator
          &                        cell,
        const unsigned int         face_no,
        const unsigned int         subface_no,
        const unsigned int         n_q_points,
        const std::vector<double> &weights,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &data,
        internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
          &output_data)
      {
        const UpdateFlags update_flags = data.update_each;

        if (update_flags &
            (update_boundary_forms | update_normal_vectors | update_jacobians |
             update_JxW_values | update_inverse_jacobians))
          {
            if (update_flags & update_boundary_forms)
              AssertDimension(output_data.boundary_forms.size(), n_q_points);
            if (update_flags & update_normal_vectors)
              AssertDimension(output_data.normal_vectors.size(), n_q_points);
            if (update_flags & update_JxW_values)
              AssertDimension(output_data.JxW_values.size(), n_q_points);

            Assert(data.aux.size() + 1 >= dim, ExcInternalError());

            // first compute some common data that is used for evaluating
            // all of the flags below

            // map the unit tangentials to the real cell. checking for d!=dim-1
            // eliminates compiler warnings regarding unsigned int expressions <
            // 0.
            for (unsigned int d = 0; d != dim - 1; ++d)
              {
                Assert(face_no + GeometryInfo<dim>::faces_per_cell * d <
                         data.unit_tangentials.size(),
                       ExcInternalError());
                Assert(
                  data.aux[d].size() <=
                    data
                      .unit_tangentials[face_no +
                                        GeometryInfo<dim>::faces_per_cell * d]
                      .size(),
                  ExcInternalError());

                mapping.transform(
                  make_array_view(
                    data
                      .unit_tangentials[face_no +
                                        GeometryInfo<dim>::faces_per_cell * d]),
                  mapping_contravariant,
                  data,
                  make_array_view(data.aux[d]));
              }

            if (update_flags & update_boundary_forms)
              {
                // if dim==spacedim, we can use the unit tangentials to compute
                // the boundary form by simply taking the cross product
                if (dim == spacedim)
                  {
                    for (unsigned int i = 0; i < n_q_points; ++i)
                      switch (dim)
                        {
                          case 1:
                            // in 1d, we don't have access to any of the
                            // data.aux fields (because it has only dim-1
                            // components), but we can still compute the
                            // boundary form by simply looking at the number of
                            // the face
                            output_data.boundary_forms[i][0] =
                              (face_no == 0 ? -1 : +1);
                            break;
                          case 2:
                            output_data.boundary_forms[i] =
                              cross_product_2d(data.aux[0][i]);
                            break;
                          case 3:
                            output_data.boundary_forms[i] =
                              cross_product_3d(data.aux[0][i], data.aux[1][i]);
                            break;
                          default:
                            Assert(false, ExcNotImplemented());
                        }
                  }
                else //(dim < spacedim)
                  {
                    // in the codim-one case, the boundary form results from the
                    // cross product of all the face tangential vectors and the
                    // cell normal vector
                    //
                    // to compute the cell normal, use the same method used in
                    // fill_fe_values for cells above
                    AssertDimension(data.contravariant.size(), n_q_points);

                    for (unsigned int point = 0; point < n_q_points; ++point)
                      {
                        if (dim == 1)
                          {
                            // J is a tangent vector
                            output_data.boundary_forms[point] =
                              data.contravariant[point].transpose()[0];
                            output_data.boundary_forms[point] /=
                              (face_no == 0 ? -1. : +1.) *
                              output_data.boundary_forms[point].norm();
                          }

                        if (dim == 2)
                          {
                            const DerivativeForm<1, spacedim, dim> DX_t =
                              data.contravariant[point].transpose();

                            Tensor<1, spacedim> cell_normal =
                              cross_product_3d(DX_t[0], DX_t[1]);
                            cell_normal /= cell_normal.norm();

                            // then compute the face normal from the face
                            // tangent and the cell normal:
                            output_data.boundary_forms[point] =
                              cross_product_3d(data.aux[0][point], cell_normal);
                          }
                      }
                  }
              }

            if (update_flags & update_JxW_values)
              for (unsigned int i = 0; i < output_data.boundary_forms.size();
                   ++i)
                {
                  output_data.JxW_values[i] =
                    output_data.boundary_forms[i].norm() * weights[i];

                  if (subface_no != numbers::invalid_unsigned_int)
                    {
                      const double area_ratio =
                        GeometryInfo<dim>::subface_ratio(
                          cell->subface_case(face_no), subface_no);
                      output_data.JxW_values[i] *= area_ratio;
                    }
                }

            if (update_flags & update_normal_vectors)
              for (unsigned int i = 0; i < output_data.normal_vectors.size();
                   ++i)
                output_data.normal_vectors[i] =
                  Point<spacedim>(output_data.boundary_forms[i] /
                                  output_data.boundary_forms[i].norm());

            if (update_flags & update_jacobians)
              for (unsigned int point = 0; point < n_q_points; ++point)
                output_data.jacobians[point] = data.contravariant[point];

            if (update_flags & update_inverse_jacobians)
              for (unsigned int point = 0; point < n_q_points; ++point)
                output_data.inverse_jacobians[point] =
                  data.covariant[point].transpose();
          }
      }


      /**
       * Do the work of MappingQGeneric::fill_fe_face_values() and
       * MappingQGeneric::fill_fe_subface_values() in a generic way,
       * using the 'data_set' to differentiate whether we will
       * work on a face (and if so, which one) or subface.
       */
      template <int dim, int spacedim>
      void
      do_fill_fe_face_values(
        const dealii::MappingQGeneric<dim, spacedim> &mapping,
        const typename dealii::Triangulation<dim, spacedim>::cell_iterator
          &                                               cell,
        const unsigned int                                face_no,
        const unsigned int                                subface_no,
        const typename QProjector<dim>::DataSetDescriptor data_set,
        const Quadrature<dim - 1> &                       quadrature,
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &data,
        internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
          &output_data)
      {
        if (dim > 1 && data.tensor_product_quadrature)
          {
            maybe_update_q_points_Jacobians_and_grads_tensor<dim, spacedim>(
              CellSimilarity::none,
              data,
              output_data.quadrature_points,
              output_data.jacobian_grads);
          }
        else
          {
            maybe_compute_q_points<dim, spacedim>(
              data_set, data, output_data.quadrature_points);
            maybe_update_Jacobians<dim, spacedim>(CellSimilarity::none,
                                                  data_set,
                                                  data);
            maybe_update_jacobian_grads<dim, spacedim>(
              CellSimilarity::none, data_set, data, output_data.jacobian_grads);
          }
        maybe_update_jacobian_pushed_forward_grads<dim, spacedim>(
          CellSimilarity::none,
          data_set,
          data,
          output_data.jacobian_pushed_forward_grads);
        maybe_update_jacobian_2nd_derivatives<dim, spacedim>(
          CellSimilarity::none,
          data_set,
          data,
          output_data.jacobian_2nd_derivatives);
        maybe_update_jacobian_pushed_forward_2nd_derivatives<dim, spacedim>(
          CellSimilarity::none,
          data_set,
          data,
          output_data.jacobian_pushed_forward_2nd_derivatives);
        maybe_update_jacobian_3rd_derivatives<dim, spacedim>(
          CellSimilarity::none,
          data_set,
          data,
          output_data.jacobian_3rd_derivatives);
        maybe_update_jacobian_pushed_forward_3rd_derivatives<dim, spacedim>(
          CellSimilarity::none,
          data_set,
          data,
          output_data.jacobian_pushed_forward_3rd_derivatives);

        maybe_compute_face_data(mapping,
                                cell,
                                face_no,
                                subface_no,
                                quadrature.size(),
                                quadrature.get_weights(),
                                data,
                                output_data);
      }
    } // namespace
  }   // namespace MappingQGenericImplementation
} // namespace internal



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::fill_fe_face_values(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const unsigned int                                          face_no,
  const Quadrature<dim - 1> &                                 quadrature,
  const typename Mapping<dim, spacedim>::InternalDataBase &   internal_data,
  internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
    &output_data) const
{
  // ensure that the following cast is really correct:
  Assert((dynamic_cast<const InternalData *>(&internal_data) != nullptr),
         ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(internal_data);

  // if necessary, recompute the support points of the transformation of this
  // cell (note that we need to first check the triangulation pointer, since
  // otherwise the second test might trigger an exception if the triangulations
  // are not the same)
  if ((data.mapping_support_points.size() == 0) ||
      (&cell->get_triangulation() !=
       &data.cell_of_current_support_points->get_triangulation()) ||
      (cell != data.cell_of_current_support_points))
    {
      data.mapping_support_points = this->compute_mapping_support_points(cell);
      data.cell_of_current_support_points = cell;
    }

  internal::MappingQGenericImplementation::do_fill_fe_face_values(
    *this,
    cell,
    face_no,
    numbers::invalid_unsigned_int,
    QProjector<dim>::DataSetDescriptor::face(ReferenceCell::get_hypercube(dim),
                                             face_no,
                                             cell->face_orientation(face_no),
                                             cell->face_flip(face_no),
                                             cell->face_rotation(face_no),
                                             quadrature.size()),
    quadrature,
    data,
    output_data);
}



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::fill_fe_subface_values(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const unsigned int                                          face_no,
  const unsigned int                                          subface_no,
  const Quadrature<dim - 1> &                                 quadrature,
  const typename Mapping<dim, spacedim>::InternalDataBase &   internal_data,
  internal::FEValuesImplementation::MappingRelatedData<dim, spacedim>
    &output_data) const
{
  // ensure that the following cast is really correct:
  Assert((dynamic_cast<const InternalData *>(&internal_data) != nullptr),
         ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(internal_data);

  // if necessary, recompute the support points of the transformation of this
  // cell (note that we need to first check the triangulation pointer, since
  // otherwise the second test might trigger an exception if the triangulations
  // are not the same)
  if ((data.mapping_support_points.size() == 0) ||
      (&cell->get_triangulation() !=
       &data.cell_of_current_support_points->get_triangulation()) ||
      (cell != data.cell_of_current_support_points))
    {
      data.mapping_support_points = this->compute_mapping_support_points(cell);
      data.cell_of_current_support_points = cell;
    }

  internal::MappingQGenericImplementation::do_fill_fe_face_values(
    *this,
    cell,
    face_no,
    subface_no,
    QProjector<dim>::DataSetDescriptor::subface(ReferenceCell::get_hypercube(
                                                  dim),
                                                face_no,
                                                subface_no,
                                                cell->face_orientation(face_no),
                                                cell->face_flip(face_no),
                                                cell->face_rotation(face_no),
                                                quadrature.size(),
                                                cell->subface_case(face_no)),
    quadrature,
    data,
    output_data);
}



namespace internal
{
  namespace MappingQGenericImplementation
  {
    namespace
    {
      template <int dim, int spacedim, int rank>
      void
      transform_fields(
        const ArrayView<const Tensor<rank, dim>> &               input,
        const MappingKind                                        mapping_kind,
        const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
        const ArrayView<Tensor<rank, spacedim>> &                output)
      {
        AssertDimension(input.size(), output.size());
        Assert((dynamic_cast<const typename dealii::
                               MappingQGeneric<dim, spacedim>::InternalData *>(
                  &mapping_data) != nullptr),
               ExcInternalError());
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &data =
            static_cast<const typename dealii::MappingQGeneric<dim, spacedim>::
                          InternalData &>(mapping_data);

        switch (mapping_kind)
          {
            case mapping_contravariant:
              {
                Assert(
                  data.update_each & update_contravariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_contravariant_transformation"));

                for (unsigned int i = 0; i < output.size(); ++i)
                  output[i] =
                    apply_transformation(data.contravariant[i], input[i]);

                return;
              }

            case mapping_piola:
              {
                Assert(
                  data.update_each & update_contravariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_contravariant_transformation"));
                Assert(
                  data.update_each & update_volume_elements,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_volume_elements"));
                Assert(rank == 1, ExcMessage("Only for rank 1"));
                if (rank != 1)
                  return;

                for (unsigned int i = 0; i < output.size(); ++i)
                  {
                    output[i] =
                      apply_transformation(data.contravariant[i], input[i]);
                    output[i] /= data.volume_elements[i];
                  }
                return;
              }
            // We still allow this operation as in the
            // reference cell Derivatives are Tensor
            // rather than DerivativeForm
            case mapping_covariant:
              {
                Assert(
                  data.update_each & update_contravariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_covariant_transformation"));

                for (unsigned int i = 0; i < output.size(); ++i)
                  output[i] = apply_transformation(data.covariant[i], input[i]);

                return;
              }

            default:
              Assert(false, ExcNotImplemented());
          }
      }


      template <int dim, int spacedim, int rank>
      void
      transform_gradients(
        const ArrayView<const Tensor<rank, dim>> &               input,
        const MappingKind                                        mapping_kind,
        const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
        const ArrayView<Tensor<rank, spacedim>> &                output)
      {
        AssertDimension(input.size(), output.size());
        Assert((dynamic_cast<const typename dealii::
                               MappingQGeneric<dim, spacedim>::InternalData *>(
                  &mapping_data) != nullptr),
               ExcInternalError());
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &data =
            static_cast<const typename dealii::MappingQGeneric<dim, spacedim>::
                          InternalData &>(mapping_data);

        switch (mapping_kind)
          {
            case mapping_contravariant_gradient:
              {
                Assert(
                  data.update_each & update_covariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_covariant_transformation"));
                Assert(
                  data.update_each & update_contravariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_contravariant_transformation"));
                Assert(rank == 2, ExcMessage("Only for rank 2"));

                for (unsigned int i = 0; i < output.size(); ++i)
                  {
                    const DerivativeForm<1, spacedim, dim> A =
                      apply_transformation(data.contravariant[i],
                                           transpose(input[i]));
                    output[i] =
                      apply_transformation(data.covariant[i], A.transpose());
                  }

                return;
              }

            case mapping_covariant_gradient:
              {
                Assert(
                  data.update_each & update_covariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_covariant_transformation"));
                Assert(rank == 2, ExcMessage("Only for rank 2"));

                for (unsigned int i = 0; i < output.size(); ++i)
                  {
                    const DerivativeForm<1, spacedim, dim> A =
                      apply_transformation(data.covariant[i],
                                           transpose(input[i]));
                    output[i] =
                      apply_transformation(data.covariant[i], A.transpose());
                  }

                return;
              }

            case mapping_piola_gradient:
              {
                Assert(
                  data.update_each & update_covariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_covariant_transformation"));
                Assert(
                  data.update_each & update_contravariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_contravariant_transformation"));
                Assert(
                  data.update_each & update_volume_elements,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_volume_elements"));
                Assert(rank == 2, ExcMessage("Only for rank 2"));

                for (unsigned int i = 0; i < output.size(); ++i)
                  {
                    const DerivativeForm<1, spacedim, dim> A =
                      apply_transformation(data.covariant[i], input[i]);
                    const Tensor<2, spacedim> T =
                      apply_transformation(data.contravariant[i],
                                           A.transpose());

                    output[i] = transpose(T);
                    output[i] /= data.volume_elements[i];
                  }

                return;
              }

            default:
              Assert(false, ExcNotImplemented());
          }
      }



      template <int dim, int spacedim>
      void
      transform_hessians(
        const ArrayView<const Tensor<3, dim>> &                  input,
        const MappingKind                                        mapping_kind,
        const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
        const ArrayView<Tensor<3, spacedim>> &                   output)
      {
        AssertDimension(input.size(), output.size());
        Assert((dynamic_cast<const typename dealii::
                               MappingQGeneric<dim, spacedim>::InternalData *>(
                  &mapping_data) != nullptr),
               ExcInternalError());
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &data =
            static_cast<const typename dealii::MappingQGeneric<dim, spacedim>::
                          InternalData &>(mapping_data);

        switch (mapping_kind)
          {
            case mapping_contravariant_hessian:
              {
                Assert(
                  data.update_each & update_covariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_covariant_transformation"));
                Assert(
                  data.update_each & update_contravariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_contravariant_transformation"));

                for (unsigned int q = 0; q < output.size(); ++q)
                  for (unsigned int i = 0; i < spacedim; ++i)
                    {
                      double tmp1[dim][dim];
                      for (unsigned int J = 0; J < dim; ++J)
                        for (unsigned int K = 0; K < dim; ++K)
                          {
                            tmp1[J][K] =
                              data.contravariant[q][i][0] * input[q][0][J][K];
                            for (unsigned int I = 1; I < dim; ++I)
                              tmp1[J][K] +=
                                data.contravariant[q][i][I] * input[q][I][J][K];
                          }
                      for (unsigned int j = 0; j < spacedim; ++j)
                        {
                          double tmp2[dim];
                          for (unsigned int K = 0; K < dim; ++K)
                            {
                              tmp2[K] = data.covariant[q][j][0] * tmp1[0][K];
                              for (unsigned int J = 1; J < dim; ++J)
                                tmp2[K] += data.covariant[q][j][J] * tmp1[J][K];
                            }
                          for (unsigned int k = 0; k < spacedim; ++k)
                            {
                              output[q][i][j][k] =
                                data.covariant[q][k][0] * tmp2[0];
                              for (unsigned int K = 1; K < dim; ++K)
                                output[q][i][j][k] +=
                                  data.covariant[q][k][K] * tmp2[K];
                            }
                        }
                    }
                return;
              }

            case mapping_covariant_hessian:
              {
                Assert(
                  data.update_each & update_covariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_covariant_transformation"));

                for (unsigned int q = 0; q < output.size(); ++q)
                  for (unsigned int i = 0; i < spacedim; ++i)
                    {
                      double tmp1[dim][dim];
                      for (unsigned int J = 0; J < dim; ++J)
                        for (unsigned int K = 0; K < dim; ++K)
                          {
                            tmp1[J][K] =
                              data.covariant[q][i][0] * input[q][0][J][K];
                            for (unsigned int I = 1; I < dim; ++I)
                              tmp1[J][K] +=
                                data.covariant[q][i][I] * input[q][I][J][K];
                          }
                      for (unsigned int j = 0; j < spacedim; ++j)
                        {
                          double tmp2[dim];
                          for (unsigned int K = 0; K < dim; ++K)
                            {
                              tmp2[K] = data.covariant[q][j][0] * tmp1[0][K];
                              for (unsigned int J = 1; J < dim; ++J)
                                tmp2[K] += data.covariant[q][j][J] * tmp1[J][K];
                            }
                          for (unsigned int k = 0; k < spacedim; ++k)
                            {
                              output[q][i][j][k] =
                                data.covariant[q][k][0] * tmp2[0];
                              for (unsigned int K = 1; K < dim; ++K)
                                output[q][i][j][k] +=
                                  data.covariant[q][k][K] * tmp2[K];
                            }
                        }
                    }

                return;
              }

            case mapping_piola_hessian:
              {
                Assert(
                  data.update_each & update_covariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_covariant_transformation"));
                Assert(
                  data.update_each & update_contravariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_contravariant_transformation"));
                Assert(
                  data.update_each & update_volume_elements,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_volume_elements"));

                for (unsigned int q = 0; q < output.size(); ++q)
                  for (unsigned int i = 0; i < spacedim; ++i)
                    {
                      double factor[dim];
                      for (unsigned int I = 0; I < dim; ++I)
                        factor[I] =
                          data.contravariant[q][i][I] / data.volume_elements[q];
                      double tmp1[dim][dim];
                      for (unsigned int J = 0; J < dim; ++J)
                        for (unsigned int K = 0; K < dim; ++K)
                          {
                            tmp1[J][K] = factor[0] * input[q][0][J][K];
                            for (unsigned int I = 1; I < dim; ++I)
                              tmp1[J][K] += factor[I] * input[q][I][J][K];
                          }
                      for (unsigned int j = 0; j < spacedim; ++j)
                        {
                          double tmp2[dim];
                          for (unsigned int K = 0; K < dim; ++K)
                            {
                              tmp2[K] = data.covariant[q][j][0] * tmp1[0][K];
                              for (unsigned int J = 1; J < dim; ++J)
                                tmp2[K] += data.covariant[q][j][J] * tmp1[J][K];
                            }
                          for (unsigned int k = 0; k < spacedim; ++k)
                            {
                              output[q][i][j][k] =
                                data.covariant[q][k][0] * tmp2[0];
                              for (unsigned int K = 1; K < dim; ++K)
                                output[q][i][j][k] +=
                                  data.covariant[q][k][K] * tmp2[K];
                            }
                        }
                    }

                return;
              }

            default:
              Assert(false, ExcNotImplemented());
          }
      }



      template <int dim, int spacedim, int rank>
      void
      transform_differential_forms(
        const ArrayView<const DerivativeForm<rank, dim, spacedim>> &input,
        const MappingKind                                        mapping_kind,
        const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
        const ArrayView<Tensor<rank + 1, spacedim>> &            output)
      {
        AssertDimension(input.size(), output.size());
        Assert((dynamic_cast<const typename dealii::
                               MappingQGeneric<dim, spacedim>::InternalData *>(
                  &mapping_data) != nullptr),
               ExcInternalError());
        const typename dealii::MappingQGeneric<dim, spacedim>::InternalData
          &data =
            static_cast<const typename dealii::MappingQGeneric<dim, spacedim>::
                          InternalData &>(mapping_data);

        switch (mapping_kind)
          {
            case mapping_covariant:
              {
                Assert(
                  data.update_each & update_contravariant_transformation,
                  typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                    "update_covariant_transformation"));

                for (unsigned int i = 0; i < output.size(); ++i)
                  output[i] = apply_transformation(data.covariant[i], input[i]);

                return;
              }
            default:
              Assert(false, ExcNotImplemented());
          }
      }
    } // namespace
  }   // namespace MappingQGenericImplementation
} // namespace internal



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::transform(
  const ArrayView<const Tensor<1, dim>> &                  input,
  const MappingKind                                        mapping_kind,
  const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
  const ArrayView<Tensor<1, spacedim>> &                   output) const
{
  internal::MappingQGenericImplementation::transform_fields(input,
                                                            mapping_kind,
                                                            mapping_data,
                                                            output);
}



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::transform(
  const ArrayView<const DerivativeForm<1, dim, spacedim>> &input,
  const MappingKind                                        mapping_kind,
  const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
  const ArrayView<Tensor<2, spacedim>> &                   output) const
{
  internal::MappingQGenericImplementation::transform_differential_forms(
    input, mapping_kind, mapping_data, output);
}



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::transform(
  const ArrayView<const Tensor<2, dim>> &                  input,
  const MappingKind                                        mapping_kind,
  const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
  const ArrayView<Tensor<2, spacedim>> &                   output) const
{
  switch (mapping_kind)
    {
      case mapping_contravariant:
        internal::MappingQGenericImplementation::transform_fields(input,
                                                                  mapping_kind,
                                                                  mapping_data,
                                                                  output);
        return;

      case mapping_piola_gradient:
      case mapping_contravariant_gradient:
      case mapping_covariant_gradient:
        internal::MappingQGenericImplementation::transform_gradients(
          input, mapping_kind, mapping_data, output);
        return;
      default:
        Assert(false, ExcNotImplemented());
    }
}



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::transform(
  const ArrayView<const DerivativeForm<2, dim, spacedim>> &input,
  const MappingKind                                        mapping_kind,
  const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
  const ArrayView<Tensor<3, spacedim>> &                   output) const
{
  AssertDimension(input.size(), output.size());
  Assert(dynamic_cast<const InternalData *>(&mapping_data) != nullptr,
         ExcInternalError());
  const InternalData &data = static_cast<const InternalData &>(mapping_data);

  switch (mapping_kind)
    {
      case mapping_covariant_gradient:
        {
          Assert(data.update_each & update_contravariant_transformation,
                 typename FEValuesBase<dim>::ExcAccessToUninitializedField(
                   "update_covariant_transformation"));

          for (unsigned int q = 0; q < output.size(); ++q)
            for (unsigned int i = 0; i < spacedim; ++i)
              for (unsigned int j = 0; j < spacedim; ++j)
                {
                  double tmp[dim];
                  for (unsigned int K = 0; K < dim; ++K)
                    {
                      tmp[K] = data.covariant[q][j][0] * input[q][i][0][K];
                      for (unsigned int J = 1; J < dim; ++J)
                        tmp[K] += data.covariant[q][j][J] * input[q][i][J][K];
                    }
                  for (unsigned int k = 0; k < spacedim; ++k)
                    {
                      output[q][i][j][k] = data.covariant[q][k][0] * tmp[0];
                      for (unsigned int K = 1; K < dim; ++K)
                        output[q][i][j][k] += data.covariant[q][k][K] * tmp[K];
                    }
                }
          return;
        }

      default:
        Assert(false, ExcNotImplemented());
    }
}



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::transform(
  const ArrayView<const Tensor<3, dim>> &                  input,
  const MappingKind                                        mapping_kind,
  const typename Mapping<dim, spacedim>::InternalDataBase &mapping_data,
  const ArrayView<Tensor<3, spacedim>> &                   output) const
{
  switch (mapping_kind)
    {
      case mapping_piola_hessian:
      case mapping_contravariant_hessian:
      case mapping_covariant_hessian:
        internal::MappingQGenericImplementation::transform_hessians(
          input, mapping_kind, mapping_data, output);
        return;
      default:
        Assert(false, ExcNotImplemented());
    }
}



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::add_line_support_points(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  std::vector<Point<spacedim>> &                              a) const
{
  // if we only need the midpoint, then ask for it.
  if (this->polynomial_degree == 2)
    {
      for (unsigned int line_no = 0;
           line_no < GeometryInfo<dim>::lines_per_cell;
           ++line_no)
        {
          const typename Triangulation<dim, spacedim>::line_iterator line =
            (dim == 1 ?
               static_cast<
                 typename Triangulation<dim, spacedim>::line_iterator>(cell) :
               cell->line(line_no));

          const Manifold<dim, spacedim> &manifold =
            ((line->manifold_id() == numbers::flat_manifold_id) &&
                 (dim < spacedim) ?
               cell->get_manifold() :
               line->get_manifold());
          a.push_back(manifold.get_new_point_on_line(line));
        }
    }
  else
    // otherwise call the more complicated functions and ask for inner points
    // from the manifold description
    {
      std::vector<Point<spacedim>> tmp_points;
      for (unsigned int line_no = 0;
           line_no < GeometryInfo<dim>::lines_per_cell;
           ++line_no)
        {
          const typename Triangulation<dim, spacedim>::line_iterator line =
            (dim == 1 ?
               static_cast<
                 typename Triangulation<dim, spacedim>::line_iterator>(cell) :
               cell->line(line_no));

          const Manifold<dim, spacedim> &manifold =
            ((line->manifold_id() == numbers::flat_manifold_id) &&
                 (dim < spacedim) ?
               cell->get_manifold() :
               line->get_manifold());

          const std::array<Point<spacedim>, 2> vertices{
            {cell->vertex(GeometryInfo<dim>::line_to_cell_vertices(line_no, 0)),
             cell->vertex(
               GeometryInfo<dim>::line_to_cell_vertices(line_no, 1))}};

          const std::size_t n_rows =
            support_point_weights_perimeter_to_interior[0].size(0);
          a.resize(a.size() + n_rows);
          auto a_view = make_array_view(a.end() - n_rows, a.end());
          manifold.get_new_points(
            make_array_view(vertices.begin(), vertices.end()),
            support_point_weights_perimeter_to_interior[0],
            a_view);
        }
    }
}



template <>
void
MappingQGeneric<3, 3>::add_quad_support_points(
  const Triangulation<3, 3>::cell_iterator &cell,
  std::vector<Point<3>> &                   a) const
{
  const unsigned int faces_per_cell = GeometryInfo<3>::faces_per_cell;

  // used if face quad at boundary or entirely in the interior of the domain
  std::vector<Point<3>> tmp_points;

  // loop over all faces and collect points on them
  for (unsigned int face_no = 0; face_no < faces_per_cell; ++face_no)
    {
      const Triangulation<3>::face_iterator face = cell->face(face_no);

#ifdef DEBUG
      const bool face_orientation          = cell->face_orientation(face_no),
                 face_flip                 = cell->face_flip(face_no),
                 face_rotation             = cell->face_rotation(face_no);
      const unsigned int vertices_per_face = GeometryInfo<3>::vertices_per_face,
                         lines_per_face    = GeometryInfo<3>::lines_per_face;

      // some sanity checks up front
      for (unsigned int i = 0; i < vertices_per_face; ++i)
        Assert(face->vertex_index(i) ==
                 cell->vertex_index(GeometryInfo<3>::face_to_cell_vertices(
                   face_no, i, face_orientation, face_flip, face_rotation)),
               ExcInternalError());

      // indices of the lines that bound a face are given by GeometryInfo<3>::
      // face_to_cell_lines
      for (unsigned int i = 0; i < lines_per_face; ++i)
        Assert(face->line(i) ==
                 cell->line(GeometryInfo<3>::face_to_cell_lines(
                   face_no, i, face_orientation, face_flip, face_rotation)),
               ExcInternalError());
#endif
      // extract the points surrounding a quad from the points
      // already computed. First get the 4 vertices and then the points on
      // the four lines
      boost::container::small_vector<Point<3>, 200> tmp_points(
        GeometryInfo<2>::vertices_per_cell +
        GeometryInfo<2>::lines_per_cell * (polynomial_degree - 1));
      for (const unsigned int v : GeometryInfo<2>::vertex_indices())
        tmp_points[v] = a[GeometryInfo<3>::face_to_cell_vertices(face_no, v)];
      if (polynomial_degree > 1)
        for (unsigned int line = 0; line < GeometryInfo<2>::lines_per_cell;
             ++line)
          for (unsigned int i = 0; i < polynomial_degree - 1; ++i)
            tmp_points[4 + line * (polynomial_degree - 1) + i] =
              a[GeometryInfo<3>::vertices_per_cell +
                (polynomial_degree - 1) *
                  GeometryInfo<3>::face_to_cell_lines(face_no, line) +
                i];

      const std::size_t n_rows =
        support_point_weights_perimeter_to_interior[1].size(0);
      a.resize(a.size() + n_rows);
      auto a_view = make_array_view(a.end() - n_rows, a.end());
      face->get_manifold().get_new_points(
        make_array_view(tmp_points.begin(), tmp_points.end()),
        support_point_weights_perimeter_to_interior[1],
        a_view);
    }
}



template <>
void
MappingQGeneric<2, 3>::add_quad_support_points(
  const Triangulation<2, 3>::cell_iterator &cell,
  std::vector<Point<3>> &                   a) const
{
  std::array<Point<3>, GeometryInfo<2>::vertices_per_cell> vertices;
  for (const unsigned int i : GeometryInfo<2>::vertex_indices())
    vertices[i] = cell->vertex(i);

  Table<2, double> weights(Utilities::fixed_power<2>(polynomial_degree - 1),
                           GeometryInfo<2>::vertices_per_cell);
  for (unsigned int q = 0, q2 = 0; q2 < polynomial_degree - 1; ++q2)
    for (unsigned int q1 = 0; q1 < polynomial_degree - 1; ++q1, ++q)
      {
        Point<2> point(line_support_points[q1 + 1][0],
                       line_support_points[q2 + 1][0]);
        for (const unsigned int i : GeometryInfo<2>::vertex_indices())
          weights(q, i) = GeometryInfo<2>::d_linear_shape_function(point, i);
      }

  const std::size_t n_rows = weights.size(0);
  a.resize(a.size() + n_rows);
  auto a_view = make_array_view(a.end() - n_rows, a.end());
  cell->get_manifold().get_new_points(
    make_array_view(vertices.begin(), vertices.end()), weights, a_view);
}



template <int dim, int spacedim>
void
MappingQGeneric<dim, spacedim>::add_quad_support_points(
  const typename Triangulation<dim, spacedim>::cell_iterator &,
  std::vector<Point<spacedim>> &) const
{
  Assert(false, ExcInternalError());
}



template <int dim, int spacedim>
std::vector<Point<spacedim>>
MappingQGeneric<dim, spacedim>::compute_mapping_support_points(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell) const
{
  // get the vertices first
  std::vector<Point<spacedim>> a;
  a.reserve(Utilities::fixed_power<dim>(polynomial_degree + 1));
  for (const unsigned int i : GeometryInfo<dim>::vertex_indices())
    a.push_back(cell->vertex(i));

  if (this->polynomial_degree > 1)
    {
      // check if all entities have the same manifold id which is when we can
      // simply ask the manifold for all points. the transfinite manifold can
      // do the interpolation better than this class, so if we detect that we
      // do not have to change anything here
      Assert(dim <= 3, ExcImpossibleInDim(dim));
      bool all_manifold_ids_are_equal = (dim == spacedim);
      if (all_manifold_ids_are_equal &&
          dynamic_cast<const TransfiniteInterpolationManifold<dim, spacedim> *>(
            &cell->get_manifold()) == nullptr)
        {
          for (auto f : GeometryInfo<dim>::face_indices())
            if (&cell->face(f)->get_manifold() != &cell->get_manifold())
              all_manifold_ids_are_equal = false;

          if (dim == 3)
            for (unsigned int l = 0; l < GeometryInfo<dim>::lines_per_cell; ++l)
              if (&cell->line(l)->get_manifold() != &cell->get_manifold())
                all_manifold_ids_are_equal = false;
        }

      if (all_manifold_ids_are_equal)
        {
          const std::size_t n_rows = support_point_weights_cell.size(0);
          a.resize(a.size() + n_rows);
          auto a_view = make_array_view(a.end() - n_rows, a.end());
          cell->get_manifold().get_new_points(make_array_view(a.begin(),
                                                              a.end() - n_rows),
                                              support_point_weights_cell,
                                              a_view);
        }
      else
        switch (dim)
          {
            case 1:
              add_line_support_points(cell, a);
              break;
            case 2:
              // in 2d, add the points on the four bounding lines to the
              // exterior (outer) points
              add_line_support_points(cell, a);

              // then get the interior support points
              if (dim != spacedim)
                add_quad_support_points(cell, a);
              else
                {
                  const std::size_t n_rows =
                    support_point_weights_perimeter_to_interior[1].size(0);
                  a.resize(a.size() + n_rows);
                  auto a_view = make_array_view(a.end() - n_rows, a.end());
                  cell->get_manifold().get_new_points(
                    make_array_view(a.begin(), a.end() - n_rows),
                    support_point_weights_perimeter_to_interior[1],
                    a_view);
                }
              break;

            case 3:
              // in 3d also add the points located on the boundary faces
              add_line_support_points(cell, a);
              add_quad_support_points(cell, a);

              // then compute the interior points
              {
                const std::size_t n_rows =
                  support_point_weights_perimeter_to_interior[2].size(0);
                a.resize(a.size() + n_rows);
                auto a_view = make_array_view(a.end() - n_rows, a.end());
                cell->get_manifold().get_new_points(
                  make_array_view(a.begin(), a.end() - n_rows),
                  support_point_weights_perimeter_to_interior[2],
                  a_view);
              }
              break;

            default:
              Assert(false, ExcNotImplemented());
              break;
          }
    }

  return a;
}



//--------------------------- Explicit instantiations -----------------------
#include "mapping_q_generic.inst"


DEAL_II_NAMESPACE_CLOSE
