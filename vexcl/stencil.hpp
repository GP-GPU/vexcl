#ifndef VEXCL_STENCIL_HPP
#define VEXCL_STENCIL_HPP

/*
The MIT License

Copyright (c) 2012 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   stencil.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  Stencil convolution.
 */

#ifdef WIN32
#  pragma warning(push)
#pragma warning (disable : 4244 4267)
#  define NOMINMAX
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
#  define __CL_ENABLE_EXCEPTIONS
#endif

#ifndef WIN32
#  define INITIALIZER_LISTS_AVAILABLE
#endif

#include <vector>
#include <map>
#include <sstream>
#include <cassert>
#include <CL/cl.hpp>
#include <vexcl/util.hpp>
#include <vexcl/vector.hpp>

namespace vex {

template <typename T>
class stencil_base {
    protected:
	template <class Iterator>
	stencil_base(
		const std::vector<cl::CommandQueue> &queue,
		uint width, uint center, Iterator begin, Iterator end
		);

	void exchange_halos(const vex::vector<T> &x) const;

	const std::vector<cl::CommandQueue> &queue;

	mutable std::vector<T>	hbuf;
	std::vector<cl::Buffer> dbuf;
	std::vector<cl::Buffer> s;
	mutable std::vector<cl::Event> event;

	int lhalo;
	int rhalo;
};

template <typename T> template <class Iterator>
stencil_base<T>::stencil_base(
	const std::vector<cl::CommandQueue> &queue,
	uint width, uint center, Iterator begin, Iterator end
	)
    : queue(queue), hbuf(queue.size() * (width - 1)),
      dbuf(queue.size()), s(queue.size()), event(queue.size()),
      lhalo(center), rhalo(width - center - 1)
{
    assert(queue.size());
    assert(lhalo >= 0);
    assert(rhalo >= 0);
    assert(width);
    assert(center < width);

    for(uint d = 0; d < queue.size(); d++) {
	cl::Context context = queue[d].getInfo<CL_QUEUE_CONTEXT>();
	cl::Device  device  = queue[d].getInfo<CL_QUEUE_DEVICE>();

	if (begin != end) {
	    s[d] = cl::Buffer(context, CL_MEM_READ_ONLY, (end - begin) * sizeof(T));

	    queue[d].enqueueWriteBuffer(s[d], CL_FALSE, 0,
		    (end - begin) * sizeof(T), &begin[0], 0, &event[d]);
	} else {
	    queue[d].enqueueMarker(&event[d]);
	}

	// Allocate one element more than needed, to be sure size is nonzero.
	dbuf[d] = cl::Buffer(context, CL_MEM_READ_WRITE, width * sizeof(T));
    }

    for(uint d = 0; d < queue.size(); d++) event[d].wait();
}

template <typename T>
void stencil_base<T>::exchange_halos(const vex::vector<T> &x) const {
    int width = lhalo + rhalo;

    if ((queue.size() <= 1) || (width <= 0)) return;

    // Get halos from neighbours.
    for(uint d = 0; d < queue.size(); d++) {
	if (!x.part_size(d)) continue;

	// Get halo from left neighbour.
	if (d > 0 && lhalo > 0) {
	    size_t end   = x.part_start(d);
	    size_t begin = end >= static_cast<uint>(lhalo) ?  end - lhalo : 0;
	    size_t size  = end - begin;
	    x.read_data(begin, size, &hbuf[d * width + lhalo - size], CL_FALSE, &event);
	}

	// Get halo from right neighbour.
	if (d + 1 < queue.size() && rhalo > 0) {
	    size_t begin = x.part_start(d + 1);
	    size_t end   = std::min(begin + rhalo, x.size());
	    size_t size  = end - begin;
	    x.read_data(begin, size, &hbuf[d * width + lhalo], CL_FALSE, &event);
	}
    }

    // Wait for the end of transfer.
    for(uint d = 0; d < queue.size(); d++) event[d].wait();

    // Write halos to a local buffer.
    for(uint d = 0; d < queue.size(); d++) {
	if (!x.part_size(d)) continue;

	if (d > 0 && lhalo > 0) {
	    size_t end   = x.part_start(d);
	    size_t begin = end >= static_cast<uint>(lhalo) ?  end - lhalo : 0;
	    size_t size  = end - begin;
	    if (size)
		std::fill(&hbuf[d * width], &hbuf[d * width + lhalo - size], hbuf[d * width + lhalo - size]);
	    else
		std::fill(&hbuf[d * width], &hbuf[d * width + lhalo - size], static_cast<T>(x[0]));
	}

	if (d + 1 < queue.size() && rhalo > 0) {
	    size_t begin = x.part_start(d + 1);
	    size_t end   = std::min(begin + rhalo, x.size());
	    size_t size  = end - begin;
	    if (size)
		std::fill(&hbuf[d * width + lhalo + size], &hbuf[(d + 1) * width], hbuf[d * width + lhalo + size - 1]);
	    else
		std::fill(&hbuf[d * width + lhalo + size], &hbuf[(d + 1) * width], static_cast<T>(x[x.size()-1]));

	}

	if ((d > 0 && lhalo > 0) || (d + 1 < queue.size() && rhalo > 0))
	    queue[d].enqueueWriteBuffer(dbuf[d], CL_FALSE, 0, width * sizeof(T),
		    &hbuf[d * width], 0, &event[d]);
    }

    // Wait for the end of transfer.
    for(uint d = 0; d < queue.size(); d++) event[d].wait();
}

/// Stencil.
/**
 * Should be used for stencil convolutions with vex::vectors as in
 * \code
 * void convolve(
 *	    const vex::stencil<double> &s,
 *	    const vex::vector<double>  &x,
 *	    vex::vector<double> &y)
 * {
 *     y = x * s;
 * }
 * \endcode
 * Stencil should be small enough to fit into local memory of all compute
 * devices it resides on.
 */
template <typename T>
class stencil : private stencil_base<T> {
    public:
	/// Costructor.
	/**
	 * \param queue  vector of queues. Each queue represents one
	 *               compute device.
	 * \param st     vector holding stencil values.
	 * \param center center of the stencil.
	 */
	stencil(const std::vector<cl::CommandQueue> &queue,
		const std::vector<T> &st, uint center
		)
	    : stencil_base<T>(queue, st.size(), center, st.begin(), st.end()),
	      conv(queue.size()), wgs(queue.size()),
	      loc_s(queue.size()), loc_x(queue.size())
	{
	    init(st.size());
	}

	/// Costructor.
	/**
	 * \param queue  vector of queues. Each queue represents one
	 *               compute device.
	 * \param begin  iterator to begin of sequence holding stencil data.
	 * \param end    iterator to end of sequence holding stencil data.
	 * \param center center of the stencil.
	 */
	template <class Iterator>
	stencil(const std::vector<cl::CommandQueue> &queue,
		Iterator begin, Iterator end, uint center
		)
	    : stencil_base<T>(queue, end - begin, center, begin, end),
	      conv(queue.size()), wgs(queue.size()),
	      loc_s(queue.size()), loc_x(queue.size())
	{
	    init(end - begin);
	}

#ifdef INITIALIZER_LISTS_AVAILABLE
	/// Costructor.
	/**
	 * \param queue  vector of queues. Each queue represents one
	 *               compute device.
	 * \param list   intializer list holding stencil values.
	 * \param center center of the stencil.
	 */
	stencil(const std::vector<cl::CommandQueue> &queue,
		std::initializer_list<T> list, uint center
		)
	    : stencil_base<T>(queue, list.size(), center, list.begin(), list.end()),
	      conv(queue.size()), wgs(queue.size()),
	      loc_s(queue.size()), loc_x(queue.size())
	{
	    init(list.size());
	}
#endif

	/// Convolve stencil with a vector.
	/**
	 * y = alpha * y + beta * conv(x);
	 * \param x input vector.
	 * \param y output vector.
	 */
	void convolve(const vex::vector<T> &x, vex::vector<T> &y,
		T alpha = 0, T beta = 1) const;
    private:
	typedef stencil_base<T> Base;

	using Base::queue;
	using Base::hbuf;
	using Base::dbuf;
	using Base::s;
	using Base::event;
	using Base::lhalo;
	using Base::rhalo;

	mutable std::vector<cl::Kernel> conv;
	std::vector<uint>               wgs;
	std::vector<cl::LocalSpaceArg>  loc_s;
	std::vector<cl::LocalSpaceArg>  loc_x;

	void init(uint width);

	static std::map<cl_context, bool>	       compiled;
	static std::map<cl_context, cl::Kernel>        slow_conv;
	static std::map<cl_context, cl::Kernel>        fast_conv;
	static std::map<cl_context, uint>	       wgsize;
};

template <typename T>
std::map<cl_context, bool> stencil<T>::compiled;

template <typename T>
std::map<cl_context, uint> stencil<T>::wgsize;

template <typename T>
std::map<cl_context, cl::Kernel> stencil<T>::slow_conv;

template <typename T>
std::map<cl_context, cl::Kernel> stencil<T>::fast_conv;

template <typename T>
struct Conv {
    Conv(const vex::vector<T> &x, const stencil<T> &s) : x(x), s(s) {}

    const vex::vector<T> &x;
    const stencil<T> &s;
};

template <typename T>
Conv<T> operator*(const vex::vector<T> &x, const stencil<T> &s) {
    return Conv<T>(x, s);
}

template <typename T>
Conv<T> operator*(const stencil<T> &s, const vex::vector<T> &x) {
    return x * s;
}

template <typename T>
const vex::vector<T>& vex::vector<T>::operator=(const Conv<T> &cnv) {
    cnv.s.convolve(cnv.x, *this);
    return *this;
}

template <class Expr, typename T>
struct ExConv {
    ExConv(const Expr &expr, const Conv<T> &cnv, T p)
	: expr(expr), cnv(cnv), p(p) {}

    const Expr    &expr;
    const Conv<T> &cnv;
    T p;
};

template <class Expr, typename T>
ExConv<Expr,T>
operator+(const Expr &expr, const Conv<T> &cnv) {
    return ExConv<Expr,T>(expr, cnv, 1);
}

template <class Expr, typename T>
ExConv<Expr,T>
operator-(const Expr &expr, const Conv<T> &cnv) {
    return ExConv<Expr,T>(expr, cnv, -1);
}

template <typename T> template <class Expr>
const vex::vector<T>& vex::vector<T>::operator=(const ExConv<Expr,T> &xc) {
    *this = xc.expr;
    xc.cnv.s.convolve(xc.cnv.x, *this, 1, xc.p);
    return *this;
}

template <typename T, uint N, bool own>
struct MultiConv {
    MultiConv(const multivector<T,N,own> &x, const stencil<T> &s) : x(x), s(s) {}

    const multivector<T,N,own> &x;
    const stencil<T>       &s;
};

template <typename T, uint N, bool own>
MultiConv<T,N,own> operator*(const multivector<T,N,own> &x, const stencil<T> &s) {
    return MultiConv<T,N,own>(x, s);
}

template <typename T, uint N, bool own>
MultiConv<T,N,own> operator*(const stencil<T> &s, const multivector<T,N,own> &x) {
    return x * s;
}

template <typename T, uint N, bool own>
const vex::multivector<T,N,own>& vex::multivector<T,N,own>::operator=(
	const MultiConv<T,N,own> &cnv)
{
    for(uint i = 0; i < N; i++)
	cnv.s.convolve(cnv.x(i), (*this)(i));
    return *this;
}

template <class Expr, typename T, uint N, bool own>
struct MultiExConv {
    MultiExConv(const Expr &expr, const MultiConv<T,N,own> &cnv, T p)
	: expr(expr), cnv(cnv), p(p) {}

    const Expr           &expr;
    const MultiConv<T,N,own> &cnv;
    T p;
};

template <class Expr, typename T, uint N, bool own>
MultiExConv<Expr,T,N,own>
operator+(const Expr &expr, const MultiConv<T,N,own> &cnv) {
    return MultiExConv<Expr,T,N,own>(expr, cnv, 1);
}

template <class Expr, typename T, uint N, bool own>
MultiExConv<Expr,T,N,own>
operator-(const Expr &expr, const MultiConv<T,N,own> &cnv) {
    return MultiExConv<Expr,T,N,own>(expr, cnv, -1);
}

template <typename T, uint N, bool own> template <class Expr>
const vex::multivector<T,N,own>& vex::multivector<T,N,own>::operator=(
	const MultiExConv<Expr,T,N,own> &xc)
{
    *this = xc.expr;
    for(uint i = 0; i < N; i++)
	xc.cnv.s.convolve(xc.cnv.x(i), (*this)(i), 1, xc.p);
    return *this;
}

template <typename T>
void stencil<T>::init(uint width) {
    for (uint d = 0; d < queue.size(); d++) {
	cl::Context context = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_CONTEXT>();
	cl::Device  device  = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_DEVICE>();

	bool device_is_cpu = device.getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU;

	if (!compiled[context()]) {
	    std::ostringstream source;

	    source << standard_kernel_header <<
		"typedef " << type_name<T>() << " real;\n"
		"real read_x(\n"
		"    long g_id,\n"
		"    " << type_name<size_t>() << " n,\n"
		"    char has_left, char has_right,\n"
		"    int lhalo, int rhalo,\n"
		"    global const real *xloc,\n"
		"    global const real *xrem\n"
		"    )\n"
		"{\n"
		"    if (g_id >= 0 && g_id < n) {\n"
		"        return xloc[g_id];\n"
		"    } else if (g_id < 0) {\n"
		"        if (has_left)\n"
		"            return (lhalo + g_id >= 0) ? xrem[lhalo + g_id] : 0;\n"
		"        else\n"
		"            return xloc[0];\n"
		"    } else {\n"
		"        if (has_right)\n"
		"            return (g_id < n + rhalo) ? xrem[lhalo + g_id - n] : 0;\n"
		"        else\n"
		"            return xloc[n - 1];\n"
		"    }\n"
		"}\n"
		"kernel void slow_conv(\n"
		"    " << type_name<size_t>() << " n,\n"
		"    char has_left,\n"
		"    char has_right,\n"
		"    int lhalo, int rhalo,\n"
		"    global const real *s,\n"
		"    global const real *xloc,\n"
		"    global const real *xrem,\n"
		"    global real *y,\n"
		"    real alpha, real beta,\n"
		"    local real *loc_s,\n"
		"    local real *loc_x\n"
		"    )\n"
		"{\n";
	    if (device_is_cpu)
		source <<
		"    long g_id = get_global_id(0);\n"
		"    if (g_id < n) {\n";
	    else
		source <<
		"    size_t grid_size = get_global_size(0);\n"
		"    for(long g_id = get_global_id(0); g_id < n; g_id += grid_size) {\n";
	    source <<
		"        real sum = 0;\n"
		"        for(int j = -lhalo; j <= rhalo; j++)\n"
		"            sum += s[lhalo + j] * read_x(g_id + j, n, has_left, has_right, lhalo, rhalo, xloc, xrem);\n"
		"        if (alpha)\n"
		"            y[g_id] = alpha * y[g_id] + beta * sum;\n"
		"        else\n"
		"            y[g_id] = beta * sum;\n"
		"    }\n"
		"}\n"
		"kernel void fast_conv(\n"
		"    " << type_name<size_t>() << " n,\n"
		"    char has_left,\n"
		"    char has_right,\n"
		"    int lhalo, int rhalo,\n"
		"    global const real *s,\n"
		"    global const real *xloc,\n"
		"    global const real *xrem,\n"
		"    global real *y,\n"
		"    real alpha, real beta,\n"
		"    local real *S,\n"
		"    local real *X\n"
		"    )\n"
		"{\n"
		"    size_t grid_size = get_global_size(0);\n"
		"    int l_id       = get_local_id(0);\n"
		"    int block_size = get_local_size(0);\n"
		"    async_work_group_copy(S, s, lhalo + rhalo + 1, 0);\n"
		"    for(long g_id = get_global_id(0), pos = 0; pos < n; g_id += grid_size, pos += grid_size) {\n"
		"        for(int i = l_id, j = g_id - lhalo; i < block_size + lhalo + rhalo; i += block_size, j += block_size)\n"
		"            X[i] = read_x(j, n, has_left, has_right, lhalo, rhalo, xloc, xrem);\n"
		"        barrier(CLK_LOCAL_MEM_FENCE);\n"
		"        if (g_id < n) {\n"
		"            real sum = 0;\n"
		"            for(int j = -lhalo; j <= rhalo; j++)\n"
		"                sum += S[lhalo + j] * X[lhalo + l_id + j];\n"
		"            if (alpha)\n"
		"                y[g_id] = alpha * y[g_id] + beta * sum;\n"
		"            else\n"
		"                y[g_id] = beta * sum;\n"
		"        }\n"
		"        barrier(CLK_LOCAL_MEM_FENCE);\n"
		"    }\n"
		"}\n";

#ifdef VEXCL_SHOW_KERNELS
	    std::cout << source.str() << std::endl;
#endif

	    auto program = build_sources(context, source.str());

	    slow_conv[context()] = cl::Kernel(program, "slow_conv");
	    fast_conv[context()] = cl::Kernel(program, "fast_conv");

	    wgsize[context()] = std::min(
		    kernel_workgroup_size(slow_conv[context()], device),
		    kernel_workgroup_size(fast_conv[context()], device)
		    );

	    compiled[context()] = true;
	}

	size_t available_lmem = (device.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>() -
		fast_conv[context()].getWorkGroupInfo<CL_KERNEL_LOCAL_MEM_SIZE>(device)
		) / sizeof(T);

	if (device_is_cpu || available_lmem < width + 64 + lhalo + rhalo) {
	    conv[d]  = slow_conv[context()];
	    wgs[d]   = wgsize[context()];
	    loc_s[d] = cl::__local(1);
	    loc_x[d] = cl::__local(1);
	} else {
	    conv[d] = fast_conv[context()];
	    wgs[d]  = wgsize[context()];
	    while(available_lmem < width + wgs[d] + lhalo + rhalo)
		wgs[d] /= 2;
	    loc_s[d] = cl::__local(sizeof(T) * width);
	    loc_x[d] = cl::__local(sizeof(T) * (wgs[d] + lhalo + rhalo));
	}

    }
}

template <typename T>
void stencil<T>::convolve(const vex::vector<T> &x, vex::vector<T> &y,
	T alpha, T beta
	) const
{
    Base::exchange_halos(x);

    for(uint d = 0; d < queue.size(); d++) {
	if (size_t psize = x.part_size(d)) {
	    cl::Context context = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_CONTEXT>();
	    cl::Device  device  = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_DEVICE>();

	    bool device_is_cpu = device.getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU;

	    char has_left  = d > 0;
	    char has_right = d + 1 < queue.size();

	    size_t g_size = device_is_cpu ? alignup(psize, wgs[d])
		: device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>() * wgs[d] * 4;

	    uint pos = 0;

	    conv[d].setArg(pos++, psize);
	    conv[d].setArg(pos++, has_left);
	    conv[d].setArg(pos++, has_right);
	    conv[d].setArg(pos++, lhalo);
	    conv[d].setArg(pos++, rhalo);
	    conv[d].setArg(pos++, s[d]);
	    conv[d].setArg(pos++, x(d));
	    conv[d].setArg(pos++, dbuf[d]);
	    conv[d].setArg(pos++, y(d));
	    conv[d].setArg(pos++, alpha);
	    conv[d].setArg(pos++, beta);
	    conv[d].setArg(pos++, loc_s[d]);
	    conv[d].setArg(pos++, loc_x[d]);

	    queue[d].enqueueNDRangeKernel(conv[d], cl::NullRange, g_size, wgs[d]);
	}
    }
}

/// Generalized stencil.
/**
 * This is generalized stencil class. Basically, this is a small dense matrix.
 * Generalized stencil may be used in combination with builtin or user
 * functions of one argument, as in
 * \code
 * vex::gstencil<double> S(ctx.queue(), 2, 3, 1, {
 *     1, -1,  0,
 *     0,  1, -1
 * });
 * y = sin(x * S);
 * \endcode
 * This notation corresponds to
 * \f$y_i = \sum_k sin(\sum_j S_{kj} * x_{i+j-c})\f$, where c is center of the
 * stencil.  Please see github wiki for further examples of this class usage.
 */
template <typename T>
class gstencil : public stencil_base<T> {
    public:
	/// Constructor.
	/**
	 * \param queue  vector of queues. Each queue represents one
	 *               compute device.
	 * \param rows   number of rows in stencil matrix.
	 * \param cols   number of cols in stencil matrix.
	 * \param center center of the stencil. This corresponds to the center
	 *		 of a row of the stencil.
	 * \param data   values of stencil matrix in row-major order.
	 */
	gstencil(const std::vector<cl::CommandQueue> &queue,
		 uint rows, uint cols, uint center,
		 const std::vector<T> &data)
	    : stencil_base<T>(queue, cols, center, data.begin(), data.end()),
	      rows(rows), cols(cols)
	{
	    assert(rows && cols);
	    assert(rows * cols == data.size());
	}

	/// Constructor.
	/**
	 * \param queue  vector of queues. Each queue represents one
	 *               compute device.
	 * \param rows   number of rows in stencil matrix.
	 * \param cols   number of cols in stencil matrix.
	 * \param center center of the stencil. This corresponds to the center
	 *		 of a row of the stencil.
	 * \param begin  iterator to begin of sequence holding stencil data.
	 * \param end    iterator to end of sequence holding stencil data.
	 */
	template <class Iterator>
	gstencil(const std::vector<cl::CommandQueue> &queue,
		 uint rows, uint cols, uint center,
		 Iterator begin, Iterator end)
	    : stencil_base<T>(queue, cols, center, begin, end),
	      rows(rows), cols(cols)
	{
	    assert(rows && cols);
	    assert(rows * cols == end - begin);
	}

#ifdef INITIALIZER_LISTS_AVAILABLE
	/// Costructor.
	/**
	 * \param queue  vector of queues. Each queue represents one
	 *               compute device.
	 * \param rows   number of rows in stencil matrix.
	 * \param cols   number of cols in stencil matrix.
	 * \param center center of the stencil. This corresponds to the center
	 *		 of a row of the stencil.
	 * \param data   values of stencil matrix in row-major order.
	 */
	gstencil(const std::vector<cl::CommandQueue> &queue,
		 uint rows, uint cols, uint center,
		 const std::initializer_list<T> &data)
	    : stencil_base<T>(queue, cols, center, data.begin(), data.end()),
	      rows(rows), cols(cols)
	{
	    assert(rows && cols);
	    assert(rows * cols == data.size());
	}
#endif

	/// Convolve stencil with a vector.
	/**
	 * \param x input vector.
	 * \param y output vector.
	 */
	template <class func_name>
	void convolve(const vex::vector<T> &x, vex::vector<T> &y,
		T alpha = 0, T beta = 1) const;
    private:
	typedef stencil_base<T> Base;

	using Base::queue;
	using Base::hbuf;
	using Base::dbuf;
	using Base::s;
	using Base::event;
	using Base::lhalo;
	using Base::rhalo;

	uint rows;
	uint cols;

	void init();

	template <class func>
	struct exdata {
	    static std::map<cl_context, bool>	    compiled;
	    static std::map<cl_context, cl::Kernel> fast_conv;
	    static std::map<cl_context, cl::Kernel> slow_conv;
	    static std::map<cl_context, uint>	    wgsize;
	};
};

template <typename T> template<class func>
std::map<cl_context, bool> gstencil<T>::exdata<func>::compiled;

template <typename T> template<class func>
std::map<cl_context, cl::Kernel> gstencil<T>::exdata<func>::slow_conv;

template <typename T> template<class func>
std::map<cl_context, cl::Kernel> gstencil<T>::exdata<func>::fast_conv;

template <typename T> template<class func>
std::map<cl_context, uint> gstencil<T>::exdata<func>::wgsize;

template <typename T>
struct GStencilProd {
    GStencilProd(const vex::vector<T> &x, const gstencil<T> &s) : x(x), s(s) {}
    const vex::vector<T> &x;
    const gstencil<T> &s;
};

template <class T>
GStencilProd<T> operator*(const vex::vector<T> &x, const gstencil<T> &s) {
    return GStencilProd<T>(x, s);
}

template <class T>
GStencilProd<T> operator*(const gstencil<T> &s, const vex::vector<T> &x) {
    return x * s;
}

template <class f, typename T>
struct GConv {
    GConv(const GStencilProd<T> &sp) : x(sp.x), s(sp.s) {}

    const vex::vector<T> &x;
    const gstencil<T> &s;
};

template <typename T, uint N, bool own>
struct MultiGStencilProd {
    MultiGStencilProd(const vex::multivector<T,N,own> &x, const gstencil<T> &s) : x(x), s(s) {}
    const vex::multivector<T,N,own> &x;
    const gstencil<T> &s;
};

template <class T, uint N, bool own>
MultiGStencilProd<T,N,own> operator*(const multivector<T,N,own> &x, const gstencil<T> &s) {
    return MultiGStencilProd<T,N,own>(x, s);
}

template <class T, uint N, bool own>
MultiGStencilProd<T,N,own> operator*(const gstencil<T> &s, const multivector<T,N,own> &x) {
    return x * s;
}

template <class f, typename T, uint N, bool own>
struct MultiGConv {
    MultiGConv(const MultiGStencilProd<T,N,own> &sp) : x(sp.x), s(sp.s) {}

    const multivector<T,N,own> &x;
    const gstencil<T> &s;
};

#define OVERLOAD_BUILTIN_FOR_GSTENCIL(name) \
template <typename T> \
GConv<name##_name,T> name(const GStencilProd<T> &s) { \
    return GConv<name##_name, T>(s); \
} \
template <typename T, uint N, bool own> \
MultiGConv<name##_name,T,N,own> name(const MultiGStencilProd<T,N,own> &s) { \
    return MultiGConv<name##_name, T, N,own>(s); \
}

OVERLOAD_BUILTIN_FOR_GSTENCIL(acos)
OVERLOAD_BUILTIN_FOR_GSTENCIL(acosh)
OVERLOAD_BUILTIN_FOR_GSTENCIL(acospi)
OVERLOAD_BUILTIN_FOR_GSTENCIL(asin)
OVERLOAD_BUILTIN_FOR_GSTENCIL(asinh)
OVERLOAD_BUILTIN_FOR_GSTENCIL(asinpi)
OVERLOAD_BUILTIN_FOR_GSTENCIL(atan)
OVERLOAD_BUILTIN_FOR_GSTENCIL(atanh)
OVERLOAD_BUILTIN_FOR_GSTENCIL(atanpi)
OVERLOAD_BUILTIN_FOR_GSTENCIL(cbrt)
OVERLOAD_BUILTIN_FOR_GSTENCIL(ceil)
OVERLOAD_BUILTIN_FOR_GSTENCIL(cos)
OVERLOAD_BUILTIN_FOR_GSTENCIL(cosh)
OVERLOAD_BUILTIN_FOR_GSTENCIL(cospi)
OVERLOAD_BUILTIN_FOR_GSTENCIL(erfc)
OVERLOAD_BUILTIN_FOR_GSTENCIL(erf)
OVERLOAD_BUILTIN_FOR_GSTENCIL(exp)
OVERLOAD_BUILTIN_FOR_GSTENCIL(exp2)
OVERLOAD_BUILTIN_FOR_GSTENCIL(exp10)
OVERLOAD_BUILTIN_FOR_GSTENCIL(expm1)
OVERLOAD_BUILTIN_FOR_GSTENCIL(fabs)
OVERLOAD_BUILTIN_FOR_GSTENCIL(floor)
OVERLOAD_BUILTIN_FOR_GSTENCIL(ilogb)
OVERLOAD_BUILTIN_FOR_GSTENCIL(lgamma)
OVERLOAD_BUILTIN_FOR_GSTENCIL(log)
OVERLOAD_BUILTIN_FOR_GSTENCIL(log2)
OVERLOAD_BUILTIN_FOR_GSTENCIL(log10)
OVERLOAD_BUILTIN_FOR_GSTENCIL(log1p)
OVERLOAD_BUILTIN_FOR_GSTENCIL(logb)
OVERLOAD_BUILTIN_FOR_GSTENCIL(nan)
OVERLOAD_BUILTIN_FOR_GSTENCIL(rint)
OVERLOAD_BUILTIN_FOR_GSTENCIL(rootn)
OVERLOAD_BUILTIN_FOR_GSTENCIL(round)
OVERLOAD_BUILTIN_FOR_GSTENCIL(rsqrt)
OVERLOAD_BUILTIN_FOR_GSTENCIL(sin)
OVERLOAD_BUILTIN_FOR_GSTENCIL(sinh)
OVERLOAD_BUILTIN_FOR_GSTENCIL(sinpi)
OVERLOAD_BUILTIN_FOR_GSTENCIL(sqrt)
OVERLOAD_BUILTIN_FOR_GSTENCIL(tan)
OVERLOAD_BUILTIN_FOR_GSTENCIL(tanh)
OVERLOAD_BUILTIN_FOR_GSTENCIL(tanpi)
OVERLOAD_BUILTIN_FOR_GSTENCIL(tgamma)
OVERLOAD_BUILTIN_FOR_GSTENCIL(trunc)

template <typename T> template <class func>
const vex::vector<T>& vex::vector<T>::operator=(const GConv<func, T> &cnv) {
    cnv.s.template convolve<func>(cnv.x, *this);
    return *this;
}

template <typename T, uint N, bool own> template <class func>
const vex::multivector<T,N,own>& vex::multivector<T,N,own>::operator=(const MultiGConv<func, T, N,own> &cnv) {
    for(uint i = 0; i < N; i++)
	cnv.s.template convolve<func>(cnv.x(i), (*this)(i));
    return *this;
}

template <class Expr, class func, typename T>
struct ExGConv {
    ExGConv(const Expr &expr, const GConv<func,T> &cnv, T p)
	: expr(expr), cnv(cnv), p(p) {}

    const Expr &expr;
    const GConv<func,T> &cnv;
    T p;
};

template <class Expr, class func, typename T>
ExGConv<Expr,func,T>
operator+(const Expr &expr, const GConv<func,T> &cnv) {
    return ExGConv<Expr,func,T>(expr, cnv, 1);
}

template <class Expr, class func, typename T>
ExGConv<Expr,func,T>
operator-(const Expr &expr, const GConv<func,T> &cnv) {
    return ExGConv<Expr,func,T>(expr, cnv, -1);
}

template <typename T> template <class Expr, class func>
const vex::vector<T>& vex::vector<T>::operator=(const ExGConv<Expr,func,T> &xc) {
    *this = xc.expr;
    xc.cnv.s.template convolve<func>(xc.cnv.x, *this, 1, xc.p);
    return *this;
}

template <class Expr, class func, typename T, uint N, bool own>
struct MultiExGConv {
    MultiExGConv(const Expr &expr, const MultiGConv<func,T,N,own> &cnv, T p)
	: expr(expr), cnv(cnv), p(p) {}

    const Expr &expr;
    const MultiGConv<func,T,N,own> &cnv;
    T p;
};

template <class Expr, class func, typename T, uint N, bool own>
MultiExGConv<Expr,func,T,N,own>
operator+(const Expr &expr, const MultiGConv<func,T,N,own> &cnv) {
    return MultiExGConv<Expr,func,T,N,own>(expr, cnv, 1);
}

template <class Expr, class func, typename T, uint N, bool own>
MultiExGConv<Expr,func,T,N,own>
operator-(const Expr &expr, const MultiGConv<func,T,N,own> &cnv) {
    return MultiExGConv<Expr,func,T,N,own>(expr, cnv, -1);
}

template <typename T, uint N, bool own> template <class Expr, class func>
const vex::multivector<T,N,own>& vex::multivector<T,N,own>::operator=(
	const MultiExGConv<Expr,func,T,N,own> &xc)
{
    *this = xc.expr;
    for(uint i = 0; i < N; i++)
	xc.cnv.s.template convolve<func>(xc.cnv.x(i), (*this)(i), 1, xc.p);
    return *this;
}

template <class T> template <class func>
void gstencil<T>::convolve(const vex::vector<T> &x, vex::vector<T> &y,
	T alpha, T beta) const
{
    for (uint d = 0; d < queue.size(); d++) {
	cl::Context context = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_CONTEXT>();
	cl::Device  device  = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_DEVICE>();

	bool device_is_cpu = device.getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU;

	if (!exdata<func>::compiled[context()]) {
	    std::ostringstream source;

	    source << standard_kernel_header <<
		"typedef " << type_name<T>() << " real;\n"
		<< UserFunctionDeclaration<func>::get() <<
		"real read_x(\n"
		"    long g_id,\n"
		"    " << type_name<size_t>() << " n,\n"
		"    char has_left, char has_right,\n"
		"    int lhalo, int rhalo,\n"
		"    global const real *xloc,\n"
		"    global const real *xrem\n"
		"    )\n"
		"{\n"
		"    if (g_id >= 0 && g_id < n) {\n"
		"        return xloc[g_id];\n"
		"    } else if (g_id < 0) {\n"
		"        if (has_left)\n"
		"            return (lhalo + g_id >= 0) ? xrem[lhalo + g_id] : 0;\n"
		"        else\n"
		"            return xloc[0];\n"
		"    } else {\n"
		"        if (has_right)\n"
		"            return (g_id < n + rhalo) ? xrem[lhalo + g_id - n] : 0;\n"
		"        else\n"
		"            return xloc[n - 1];\n"
		"    }\n"
		"}\n"
		"kernel void slow_conv(\n"
		"    " << type_name<size_t>() << " n,\n"
		"    char has_left,\n"
		"    char has_right,\n"
		"    uint rows, uint cols,\n"
		"    int lhalo, int rhalo,\n"
		"    global const real *s,\n"
		"    global const real *xloc,\n"
		"    global const real *xrem,\n"
		"    global real *y,\n"
		"    real alpha, real beta,\n"
		"    local real *loc_s,\n"
		"    local real *loc_x\n"
		"    )\n"
		"{\n";
	    if (device_is_cpu)
		source <<
		"    long g_id = get_global_id(0);\n"
		"    if (g_id < n) {\n";
	    else
		source <<
		"    size_t grid_size = get_global_size(0);\n"
		"    for(long g_id = get_global_id(0); g_id < n; g_id += grid_size) {\n";
	    source <<
		"        real srow = 0;\n"
		"        for(int k = 0; k < rows; k++) {\n"
		"            real scol = 0;\n"
		"            for(int j = -lhalo; j <= rhalo; j++)\n"
		"                scol += s[lhalo + j + k * cols] * read_x(g_id + j, n, has_left, has_right, lhalo, rhalo, xloc, xrem);\n"
		"            srow += " << func::value() << "(scol);\n"
		"        }\n"
		"        if (alpha)\n"
		"            y[g_id] = alpha * y[g_id] + beta * srow;\n"
		"        else\n"
		"            y[g_id] = beta * srow;\n"
		"    }\n"
		"}\n"
		"kernel void fast_conv(\n"
		"    " << type_name<size_t>() << " n,\n"
		"    char has_left,\n"
		"    char has_right,\n"
		"    uint rows, uint cols,\n"
		"    int lhalo, int rhalo,\n"
		"    global const real *s,\n"
		"    global const real *xloc,\n"
		"    global const real *xrem,\n"
		"    global real *y,\n"
		"    real alpha, real beta,\n"
		"    local real *S,\n"
		"    local real *X\n"
		"    )\n"
		"{\n"
		"    size_t grid_size = get_global_size(0);\n"
		"    int l_id       = get_local_id(0);\n"
		"    int block_size = get_local_size(0);\n"
		"    async_work_group_copy(S, s, rows * cols, 0);\n"
		"    for(long g_id = get_global_id(0), pos = 0; pos < n; g_id += grid_size, pos += grid_size) {\n"
		"        for(int i = l_id, j = g_id - lhalo; i < block_size + lhalo + rhalo; i += block_size, j += block_size)\n"
		"            X[i] = read_x(j, n, has_left, has_right, lhalo, rhalo, xloc, xrem);\n"
		"        barrier(CLK_LOCAL_MEM_FENCE);\n"
		"        if (g_id < n) {\n"
		"            real srow = 0;\n"
		"            for(int k = 0; k < rows; k++) {\n"
		"                real scol = 0;\n"
		"                for(int j = -lhalo; j <= rhalo; j++)\n"
		"                    scol += S[lhalo + j + k * cols] * X[lhalo + l_id + j];\n"
		"                srow += " << func::value() << "(scol);\n"
		"            }\n"
		"            if (alpha)\n"
		"                y[g_id] = alpha * y[g_id] + beta * srow;\n"
		"            else\n"
		"                y[g_id] = beta * srow;\n"
		"        }\n"
		"        barrier(CLK_LOCAL_MEM_FENCE);\n"
		"    }\n"
		"}\n";

#ifdef VEXCL_SHOW_KERNELS
	    std::cout << source.str() << std::endl;
#endif

	    auto program = build_sources(context, source.str());

	    exdata<func>::slow_conv[context()] = cl::Kernel(program, "slow_conv");
	    exdata<func>::fast_conv[context()] = cl::Kernel(program, "fast_conv");

	    exdata<func>::wgsize[context()] = std::min(
		    kernel_workgroup_size(exdata<func>::slow_conv[context()], device),
		    kernel_workgroup_size(exdata<func>::fast_conv[context()], device)
		    );

	    exdata<func>::compiled[context()] = true;
	}
    }

    Base::exchange_halos(x);

    for(uint d = 0; d < queue.size(); d++) {
	if (size_t psize = x.part_size(d)) {
	    cl::Context context = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_CONTEXT>();
	    cl::Device  device  = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_DEVICE>();

	    bool device_is_cpu = device.getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU;

	    size_t available_lmem = (device.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>() -
		    static_cast<cl::Kernel>(exdata<func>::fast_conv[context()]
			).getWorkGroupInfo<CL_KERNEL_LOCAL_MEM_SIZE>(device)
		    ) / sizeof(T);

	    cl::Kernel conv;
	    uint wgs;
	    cl::LocalSpaceArg loc_s;
	    cl::LocalSpaceArg loc_x;

	    if (device_is_cpu || available_lmem < rows * cols + 64 + lhalo + rhalo) {
		conv  = exdata<func>::slow_conv[context()];
		wgs   = exdata<func>::wgsize[context()];
		loc_s = cl::__local(1);
		loc_x = cl::__local(1);
	    } else {
		conv = exdata<func>::fast_conv[context()];
		wgs  = exdata<func>::wgsize[context()];
		while(available_lmem < rows * cols + wgs + lhalo + rhalo)
		    wgs /= 2;
		loc_s = cl::__local(sizeof(T) * cols * rows);
		loc_x = cl::__local(sizeof(T) * (wgs + lhalo + rhalo));
	    }

	    char has_left  = d > 0;
	    char has_right = d + 1 < queue.size();

	    size_t g_size = device_is_cpu ? alignup(psize, wgs)
		: device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>() * wgs * 4;

	    uint pos = 0;

	    conv.setArg(pos++, psize);
	    conv.setArg(pos++, has_left);
	    conv.setArg(pos++, has_right);
	    conv.setArg(pos++, rows);
	    conv.setArg(pos++, cols);
	    conv.setArg(pos++, lhalo);
	    conv.setArg(pos++, rhalo);
	    conv.setArg(pos++, s[d]);
	    conv.setArg(pos++, x(d));
	    conv.setArg(pos++, dbuf[d]);
	    conv.setArg(pos++, y(d));
	    conv.setArg(pos++, alpha);
	    conv.setArg(pos++, beta);
	    conv.setArg(pos++, loc_s);
	    conv.setArg(pos++, loc_x);

	    queue[d].enqueueNDRangeKernel(conv, cl::NullRange, g_size, wgs);
	}
    }
}

template <typename T, uint width, uint center, const char *body>
struct OperConv;

template <typename T, uint N, bool own, uint width, uint center, const char *body>
struct MultiOperConv;

/// User-defined stencil operator
/**
 * Is used to define custom stencil operator. For example, to implement the
 * following nonlinear operator:
 * \code
 * y[i] = x[i] + pow3(x[i-1] + x[i+1]);
 * \endcode
 * one has to write:
 * \code
 * extern const char pow3_oper_body[] = "return X[0] + pow(X[-1] + X[1], 3);";
 * StencilOperator<double, 3, 1, pow3_oper_body> pow3_oper(ctx.queue());
 * 
 * y = pow3_oper(x);
 * \endcode
 */
template <typename T, uint width, uint center, const char *body>
class StencilOperator : public stencil_base<T> {
    public:
	StencilOperator(const std::vector<cl::CommandQueue> &queue);

	OperConv<T, width, center, body> operator()(const vex::vector<T> &x) const;

	template <uint N, bool own>
	MultiOperConv<T, N,own, width, center, body> operator()(const vex::multivector<T, N,own> &x) const;

	void convolve(const vex::vector<T> &x, vex::vector<T> &y,
		T alpha = 0, T beta = 1) const;
    private:
	typedef stencil_base<T> Base;

	using Base::queue;
	using Base::hbuf;
	using Base::dbuf;
	using Base::event;
	using Base::lhalo;
	using Base::rhalo;

	static std::map<cl_context, bool>	       compiled;
	static std::map<cl_context, cl::Kernel>        kernel;
	static std::map<cl_context, uint>	       wgsize;
	static std::map<cl_context, cl::LocalSpaceArg> lmem;
};

template <typename T, uint width, uint center, const char *body>
struct OperConv {
    OperConv(const StencilOperator<T, width, center, body> &op, const vex::vector<T> &x)
	: op(op), x(x) {}

    const StencilOperator<T, width, center, body> &op;
    const vex::vector<T> &x;
};

template <typename T, uint N, bool own, uint width, uint center, const char *body>
struct MultiOperConv {
    MultiOperConv(const multivector<T,N,own> &x, const StencilOperator<T, width, center, body> &s)
	: op(s), x(x) {}

    const StencilOperator<T, width, center, body> &op;
    const multivector<T,N,own> &x;
};

template <typename T, uint width, uint center, const char *body>
std::map<cl_context, bool> StencilOperator<T, width, center, body>::compiled;

template <typename T, uint width, uint center, const char *body>
std::map<cl_context, cl::Kernel> StencilOperator<T, width, center, body>::kernel;

template <typename T, uint width, uint center, const char *body>
std::map<cl_context, uint> StencilOperator<T, width, center, body>::wgsize;

template <typename T, uint width, uint center, const char *body>
std::map<cl_context, cl::LocalSpaceArg> StencilOperator<T, width, center, body>::lmem;

template <typename T> template <uint width, uint center, const char *body>
const vex::vector<T>& vex::vector<T>::operator=(const OperConv<T, width, center, body> &cnv) {
    cnv.op.convolve(cnv.x, *this);
    return *this;
}

template <class Expr, typename T, uint width, uint center, const char *body>
struct ExOperConv {
    ExOperConv(const Expr &expr, const OperConv<T, width, center, body> &cnv, T p)
	: expr(expr), cnv(cnv), p(p) {}

    const Expr    &expr;
    const OperConv<T, width, center, body> &cnv;
    T p;
};

template <class Expr, typename T, uint width, uint center, const char *body>
ExOperConv<Expr, T, width, center, body>
operator+(const Expr &expr, const OperConv<T, width, center, body> &cnv) {
    return ExOperConv<Expr, T, width, center, body>(expr, cnv, 1);
}

template <class Expr, typename T, uint width, uint center, const char *body>
ExOperConv<Expr, T, width, center, body>
operator-(const Expr &expr, const OperConv<T, width, center, body> &cnv) {
    return ExOperConv<Expr, T, width, center, body>(expr, cnv, -1);
}

template <typename T> template <class Expr, uint width, uint center, const char *body>
const vex::vector<T>& vex::vector<T>::operator=(const ExOperConv<Expr, T, width, center, body> &xc) {
    *this = xc.expr;
    xc.cnv.op.convolve(xc.cnv.x, *this, 1, xc.p);
    return *this;
}

template <typename T, uint N, bool own> template<uint width, uint center, const char *body>
const vex::multivector<T,N,own>& vex::multivector<T,N,own>::operator=(
	const MultiOperConv<T, N,own, width, center, body> &cnv)
{
    for(uint i = 0; i < N; i++) cnv.op.convolve(cnv.x(i), (*this)(i));
    return *this;
}

template <class Expr, typename T, uint N, bool own, uint width, uint center, const char *body>
struct MultiExOperConv {
    MultiExOperConv(const Expr &expr, const MultiOperConv<T, N,own, width, center, body> &cnv, T p)
	: expr(expr), cnv(cnv), p(p) {}

    const Expr &expr;
    const MultiOperConv<T, N,own, width, center, body> &cnv;
    T p;
};

template <class Expr, typename T, uint N, bool own, uint width, uint center, const char *body>
MultiExOperConv<Expr, T, N,own, width, center, body>
operator+(const Expr &expr, const MultiOperConv<T, N,own, width, center, body> &cnv) {
    return MultiExOperConv<Expr, T, N,own, width, center, body>(expr, cnv, 1);
}

template <class Expr, typename T, uint N, bool own, uint width, uint center, const char *body>
MultiExOperConv<Expr, T, N,own, width, center, body>
operator-(const Expr &expr, const MultiOperConv<T, N,own, width, center, body> &cnv) {
    return MultiExOperConv<Expr, T, N,own, width, center, body>(expr, cnv, -1);
}

template <typename T, uint N, bool own> template <class Expr, uint width, uint center, const char *body>
const vex::multivector<T,N,own>& vex::multivector<T,N,own>::operator=(
	const MultiExOperConv<Expr, T, N,own, width, center, body> &xc)
{
    *this = xc.expr;
    for(uint i = 0; i < N; i++)
	xc.cnv.op.convolve(xc.cnv.x(i), (*this)(i), 1, xc.p);
    return *this;
}

template <typename T, uint width, uint center, const char *body>
StencilOperator<T, width, center, body>::StencilOperator(
	const std::vector<cl::CommandQueue> &queue)
    : Base(queue, width, center, static_cast<T*>(0), static_cast<T*>(0))
{
    for (uint d = 0; d < queue.size(); d++) {
	cl::Context context = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_CONTEXT>();
	cl::Device  device  = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_DEVICE>();

	if (!compiled[context()]) {
	    bool device_is_cpu = device.getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU;

	    std::ostringstream source;

	    source << standard_kernel_header <<
		"typedef " << type_name<T>() << " real;\n"
		"real read_x(\n"
		"    long g_id,\n"
		"    " << type_name<size_t>() << " n,\n"
		"    char has_left, char has_right,\n"
		"    int lhalo, int rhalo,\n"
		"    global const real *xloc,\n"
		"    global const real *xrem\n"
		"    )\n"
		"{\n"
		"    if (g_id >= 0 && g_id < n) {\n"
		"        return xloc[g_id];\n"
		"    } else if (g_id < 0) {\n"
		"        if (has_left)\n"
		"            return (lhalo + g_id >= 0) ? xrem[lhalo + g_id] : 0;\n"
		"        else\n"
		"            return xloc[0];\n"
		"    } else {\n"
		"        if (has_right)\n"
		"            return (g_id < n + rhalo) ? xrem[lhalo + g_id - n] : 0;\n"
		"        else\n"
		"            return xloc[n - 1];\n"
		"    }\n"
		"}\n"
		"real stencil_oper(local real *X) {\n"
		<< body <<
		"}\n"
		"kernel void convolve(\n"
		"    " << type_name<size_t>() << " n,\n"
		"    char has_left,\n"
		"    char has_right,\n"
		"    int lhalo, int rhalo,\n"
		"    global const real *xloc,\n"
		"    global const real *xrem,\n"
		"    global real *y,\n"
		"    real alpha, real beta,\n"
		"    local real *X\n"
		"    )\n"
		"{\n";
	    if (device_is_cpu)
		source <<
		"    int l_id       = get_local_id(0);\n"
		"    int block_size = get_local_size(0);\n"
		"    long g_id      = get_global_id(0);\n"
		"    if (g_id < n) {\n"
		"        for(int i = 0, j = g_id - lhalo; i < 1 + lhalo + rhalo; i++, j++)\n"
		"            X[i] = read_x(j, n, has_left, has_right, lhalo, rhalo, xloc, xrem);\n"
		"        real sum = stencil_oper(X + lhalo);\n"
		"        if (alpha)\n"
		"            y[g_id] = alpha * y[g_id] + beta * sum;\n"
		"        else\n"
		"            y[g_id] = beta * sum;\n"
		"    }\n"
		"}\n";
	    else
		source <<
		"    size_t grid_size = get_global_size(0);\n"
		"    int l_id         = get_local_id(0);\n"
		"    int block_size   = get_local_size(0);\n"
		"    for(long g_id = get_global_id(0), pos = 0; pos < n; g_id += grid_size, pos += grid_size) {\n"
		"        for(int i = l_id, j = g_id - lhalo; i < block_size + lhalo + rhalo; i += block_size, j += block_size)\n"
		"            X[i] = read_x(j, n, has_left, has_right, lhalo, rhalo, xloc, xrem);\n"
		"        barrier(CLK_LOCAL_MEM_FENCE);\n"
		"        if (g_id < n) {\n"
		"            real sum = stencil_oper(X + lhalo + l_id);\n"
		"            if (alpha)\n"
		"                y[g_id] = alpha * y[g_id] + beta * sum;\n"
		"            else\n"
		"                y[g_id] = beta * sum;\n"
		"        }\n"
		"        barrier(CLK_LOCAL_MEM_FENCE);\n"
		"    }\n"
		"}\n";

#ifdef VEXCL_SHOW_KERNELS
	    std::cout << source.str() << std::endl;
#endif

	    auto program = build_sources(context, source.str());

	    kernel[context()]   = cl::Kernel(program, "convolve");
	    wgsize[context()]   = kernel_workgroup_size(kernel[context()], device);
	    compiled[context()] = true;

	    size_t available_lmem = (device.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>() -
		    kernel[context()].getWorkGroupInfo<CL_KERNEL_LOCAL_MEM_SIZE>(device)
		    ) / sizeof(T);

	    assert(available_lmem >= width + 64);

	    while(available_lmem < width + wgsize[context()])
		wgsize[context()] /= 2;

	    lmem[context()] = device_is_cpu ? cl::__local(width)
		: cl::__local(sizeof(T) * (wgsize[context()] + width - 1));
	}

    }
}

template <typename T, uint width, uint center, const char *body>
OperConv<T, width, center, body> StencilOperator<T, width, center, body>::operator()(
	const vex::vector<T> &x) const
{
    return OperConv<T, width, center, body>(*this, x);
}

template <typename T, uint width, uint center, const char *body> template <uint N, bool own>
MultiOperConv<T, N,own, width, center, body> StencilOperator<T, width, center, body>::operator()(
	const vex::multivector<T, N,own> &x) const
{
    return MultiOperConv<T, N,own, width, center, body>(*this, x);
}

template <typename T, uint width, uint center, const char *body>
void StencilOperator<T, width, center, body>::convolve(
	const vex::vector<T> &x, vex::vector<T> &y, T alpha, T beta) const
{
    Base::exchange_halos(x);

    for(uint d = 0; d < queue.size(); d++) {
	if (size_t psize = x.part_size(d)) {
	    cl::Context context = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_CONTEXT>();
	    cl::Device  device  = static_cast<cl::CommandQueue>(queue[d]).getInfo<CL_QUEUE_DEVICE>();

	    bool device_is_cpu = device.getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU;

	    size_t g_size = device_is_cpu ? alignup(psize, wgsize[context()]) :
		device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>() * wgsize[context()] * 4;

	    char has_left  = d > 0;
	    char has_right = d + 1 < queue.size();

	    uint pos = 0;

	    kernel[context()].setArg(pos++, psize);
	    kernel[context()].setArg(pos++, has_left);
	    kernel[context()].setArg(pos++, has_right);
	    kernel[context()].setArg(pos++, lhalo);
	    kernel[context()].setArg(pos++, rhalo);
	    kernel[context()].setArg(pos++, x(d));
	    kernel[context()].setArg(pos++, dbuf[d]);
	    kernel[context()].setArg(pos++, y(d));
	    kernel[context()].setArg(pos++, alpha);
	    kernel[context()].setArg(pos++, beta);
	    kernel[context()].setArg(pos++, lmem[context()]);

	    queue[d].enqueueNDRangeKernel(kernel[context()], cl::NullRange, g_size, wgsize[context()]);
	}
    }
}

} // namespace vex

#ifdef WIN32
#  pragma warning(pop)
#endif
#endif
