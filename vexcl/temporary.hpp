#ifndef VEXCL_TEMPORARY_HPP
#define VEXCL_TEMPORARY_HPP

/*
The MIT License

Copyright (c) 2012-2013 Denis Demidov <ddemidov@ksu.ru>

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
 * \file   vexcl/temporary.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  Container for intermediate results.
 */

#include <set>
#include <vexcl/operations.hpp>

namespace vex {

/// \cond INTERNAL
struct temporary_terminal {};

typedef vector_expression<
    typename boost::proto::terminal< temporary_terminal >::type
    > temporary_terminal_expression;

template <typename T, size_t Tag, class Expr>
struct temporary : public temporary_terminal_expression
{
    const Expr expr;

    temporary(const Expr &expr) : expr(expr) {}
};

/// \endcond

/// Create temporary to be reused in a vector expression.
template <typename T, size_t Tag, class Expr>
temporary<T, Tag, Expr> make_temp(const Expr &expr) {
    return temporary<T, Tag, Expr>(expr);
}

/// \cond INTERNAL

namespace traits {

template <>
struct is_vector_expr_terminal< temporary_terminal > : std::true_type {};

template <typename T, size_t Tag, class Expr>
struct terminal_preamble< temporary<T, Tag, Expr> > {
    static std::string get(const temporary<T, Tag, Expr> &term,
            const cl::Device &dev, const std::string &prm_name,
            detail::kernel_generator_state &state)
    {
        auto s = state.find("temporary");

        if (s == state.end()) {
            s = state.insert(std::make_pair(
                        std::string("temporary"),
                        boost::any(std::set<size_t>())
                        )).first;
        }

        auto &pos = boost::any_cast< std::set<size_t>& >(s->second);
        auto p = pos.find(Tag);

        if (p == pos.end()) {
            pos.insert(Tag);

            std::ostringstream s;

            detail::output_terminal_preamble termpream(s, dev, 1, prm_name + "_temp_");
            boost::proto::eval(boost::proto::as_child(term.expr), termpream);

            return s.str();
        } else {
            return "";
        }
    }
};

template <typename T, size_t Tag, class Expr>
struct kernel_param_declaration< temporary<T, Tag, Expr> > {
    static std::string get(const temporary<T, Tag, Expr> &term,
            const cl::Device &dev, const std::string &prm_name,
            detail::kernel_generator_state &state)
    {
        auto s = state.find("temporary");

        if (s == state.end()) {
            s = state.insert(std::make_pair(
                        std::string("temporary"),
                        boost::any(std::set<size_t>())
                        )).first;
        }

        auto &pos = boost::any_cast< std::set<size_t>& >(s->second);
        auto p = pos.find(Tag);

        if (p == pos.end()) {
            pos.insert(Tag);

            std::ostringstream s;

            detail::declare_expression_parameter declare(s, dev, 1, prm_name + "_temp_");
            detail::extract_terminals()(boost::proto::as_child(term.expr),  declare);

            return s.str();
        } else {
            return "";
        }
    }
};

template <typename T, size_t Tag, class Expr>
struct local_terminal_init< temporary<T, Tag, Expr> > {
    static std::string get(const temporary<T, Tag, Expr> &term,
            const cl::Device &dev, const std::string &prm_name,
            detail::kernel_generator_state &state)
    {
        auto s = state.find("temporary");

        if (s == state.end()) {
            s = state.insert(std::make_pair(
                        std::string("temporary"),
                        boost::any(std::set<size_t>())
                        )).first;
        }

        auto &pos = boost::any_cast< std::set<size_t>& >(s->second);
        auto p = pos.find(Tag);

        if (p == pos.end()) {
            pos.insert(Tag);

            std::ostringstream s;

            s << "\t\t" << type_name<T>() << " temp_" << Tag << " = ";

            detail::vector_expr_context expr_ctx(s, dev, 1, prm_name + "_temp_");
            boost::proto::eval(boost::proto::as_child(term.expr), expr_ctx);
            s << ";\n";

            return s.str();
        } else {
            return "";
        }
    }
};

template <typename T, size_t Tag, class Expr>
struct partial_vector_expr< temporary<T, Tag, Expr> > {
    static std::string get(const temporary<T, Tag, Expr>&,
            const cl::Device&, const std::string &prm_name,
            detail::kernel_generator_state &state)
    {
        auto s = state.find("temporary");

        if (s == state.end()) {
            s = state.insert(std::make_pair(
                        std::string("temporary"),
                        boost::any(std::map<size_t, std::string>())
                        )).first;
        }

        auto &pos = boost::any_cast< std::map<size_t, std::string>& >(s->second);
        auto p = pos.find(Tag);

        if (p == pos.end()) {
            return (pos[Tag] = std::string("temp_") + std::to_string(Tag));
        } else {
            return p->second;
        }
    }
};

template <typename T, size_t Tag, class Expr>
struct kernel_arg_setter< temporary<T, Tag, Expr> > {
    static void set(const temporary<T, Tag, Expr> &term,
            cl::Kernel &kernel, unsigned device, size_t index_offset,
            unsigned &position, detail::kernel_generator_state &state)
    {
        auto s = state.find("temporary");

        if (s == state.end()) {
            s = state.insert(std::make_pair(
                        std::string("temporary"),
                        boost::any(std::set<size_t>())
                        )).first;
        }

        auto &pos = boost::any_cast< std::set<size_t>& >(s->second);
        auto p = pos.find(Tag);

        if (p == pos.end()) {
            pos.insert(Tag);

            detail::set_expression_argument setarg(kernel, device, position, index_offset);
            detail::extract_terminals()( boost::proto::as_child(term.expr),  setarg);
        }
    }
};

template <typename T, size_t Tag, class Expr>
struct expression_properties< temporary<T, Tag, Expr> > {
    static void get(const temporary<T, Tag, Expr> &term,
            std::vector<cl::CommandQueue> &queue_list,
            std::vector<size_t> &partition,
            size_t &size
            )
    {
        detail::get_expression_properties prop;
        detail::extract_terminals()(boost::proto::as_child(term.expr), prop);

        queue_list = prop.queue;
        partition  = prop.part;
        size       = prop.size;
    }
};
} // namespace traits

/// \endcond

} // namespace vex

#endif