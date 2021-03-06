/*
Copyright (c) 2015-2018, Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

//////////////////////////////////////////////////////////////////////////////

#include <cassert>
#include <sstream>

#include "options.h"
#include "sym_table.h"
#include "type.h"
#include "util.h"
#include "variable.h"

using namespace yarpgen;

// FIXME: Check for tiger syntax
std::string Type::get_name () {
    std::string ret = "";
    ret += name;
    return ret;
}

StructType::StructMember::StructMember (std::shared_ptr<Type> _type, std::string _name) : type(_type), name(_name), data(nullptr) {
    if (!type->get_is_static())
        return;
    if (type->is_int_type())
        data = std::make_shared<ScalarVariable>(name, std::static_pointer_cast<IntegerType>(type));
    else if (type->is_struct_type())
        data = std::make_shared<Struct>(name, std::static_pointer_cast<StructType>(type));
    else {
        ERROR("unsupported data type (StructType)");
    }
}

void StructType::add_member (std::shared_ptr<Type> _type, std::string _name) {
    StructType::StructMember new_mem (_type, _name);
    if (_type->is_struct_type()) {
        nest_depth = std::static_pointer_cast<StructType>(_type)->get_nest_depth() >= nest_depth ?
                     std::static_pointer_cast<StructType>(_type)->get_nest_depth() + 1 : nest_depth;
    }
    members.push_back(std::make_shared<StructMember>(new_mem));
    shadow_members.push_back(std::make_shared<StructMember>(new_mem));
}

std::shared_ptr<StructType::StructMember> StructType::get_member (unsigned int num) {
    if (num >= members.size())
        return nullptr;
    else
        return members.at(num);
}

std::string StructType::StructMember::get_definition (std::string offset) {
    std::string ret = offset + type->get_name() + " " + name;
    if (type->get_is_bit_field())
        ret += " : " + std::to_string(std::static_pointer_cast<BitField>(type)->get_bit_field_width());
    return ret;
}

// FIXME: Check for tiger syntax
std::string StructType::get_definition (std::string offset) {
    std::string ret = "";
    ret += "type ";
    if (options->is_cxx())
        ret += name;
    ret += " {\n";
    for (auto i : shadow_members) {
        ret += i->get_definition(offset + "    ") + ",\n";
    }
    ret += "}";
    if (options->is_c())
        ret += " " + name;
    ret += "\n";
    return ret;
}

// This function implements single iteration of loop of static members' initialization emission
static std::string static_memb_init_iter(std::shared_ptr<Data> member) {
    std::string ret;
    if (member->get_class_id() == Data::VAR) {
        ConstExpr init_expr(std::static_pointer_cast<ScalarVariable>(member)->get_init_value());
        std::stringstream sstream;
        init_expr.emit(sstream);
        ret += sstream.str();
    } else if (member->get_class_id() == Data::STRUCT) {
        std::shared_ptr<Struct> member_struct = std::static_pointer_cast<Struct>(member);
        // Recursively walk over all members
        // FIXME: Check for tiger syntax
        ret += "{";
        uint64_t member_count = member_struct->get_member_count();
        for (unsigned int i = 0; i < member_count; ++i) {
            std::shared_ptr<Data> cur_member = member_struct->get_member(i);
            if (cur_member->get_type()->get_is_static())
                continue;
            ret += static_memb_init_iter(cur_member) + (i != member_count - 1 ? ", " : "");
        }
        ret += "}";
    } else
        ERROR("bad Data::ClassID");

    return ret;
}

std::string StructType::get_static_memb_def (std::string offset) {
    std::string ret;
    for (const auto& i : members)
        if (i->get_type()->get_is_static())
            ret += offset + i->get_type()->get_simple_name() + " " + name + "::" + i->get_name() + " = " +
                   static_memb_init_iter(i->get_data()) + ";\n";
    return ret;
}

// This function implements single iteration of loop of static members' check emission
static std::string static_memb_check_iter(std::string offset, std::string parent_str, std::shared_ptr<Data> member) {
    std::string ret;
    parent_str = parent_str + member->get_name();
    if (member->get_class_id() == Data::VAR)
        // FIXME: Check for tiger syntax
        ret += offset + "hash(&seed, " + parent_str + ");\n";
    else if (member->get_class_id() == Data::STRUCT) {
        std::shared_ptr<Struct> member_struct = std::static_pointer_cast<Struct>(member);
        // Recursively walk over all members
        for (unsigned int i = 0; i < member_struct->get_member_count(); ++i) {
            std::shared_ptr<Data> cur_member = member_struct->get_member(i);
            if (cur_member->get_type()->get_is_static())
                continue;
            ret += static_memb_check_iter(offset, parent_str + ".", cur_member);
        }
    } else
        ERROR("bad Data::ClassID");

    return ret;
}

std::string StructType::get_static_memb_check (std::string offset) {
    std::string ret;
    for (const auto& i : members)
        if (i->get_type()->get_is_static())
        ret += static_memb_check_iter(offset, name + "::", i->get_data());
    return ret;
}

void StructType::dbg_dump() {
    std::cout << get_definition () << std::endl;
    std::cout << "depth: " << nest_depth << std::endl;
}

std::shared_ptr<StructType> StructType::generate (std::shared_ptr<Context> ctx) {
    std::vector<std::shared_ptr<StructType>> empty_vec;
    return generate(ctx, empty_vec);
}

std::shared_ptr<StructType> StructType::generate (std::shared_ptr<Context> ctx,
                                                  std::vector<std::shared_ptr<StructType>> nested_struct_types) {
    auto p = ctx->get_gen_policy();
    Type::CV_Qual primary_cv_qual = rand_val_gen->get_rand_elem(p->get_allowed_cv_qual());

    bool primary_static_spec = false;
    //TODO: add distr to gen_policy
    if (p->get_allow_static_var())
        primary_static_spec = rand_val_gen->get_rand_value(false, true);

    IntegerType::IntegerTypeID int_type_id = rand_val_gen->get_rand_id(p->get_allowed_int_types());
    //TODO: what about align?
    std::shared_ptr<Type> primary_type = IntegerType::init(int_type_id, primary_cv_qual, primary_static_spec, 0);

    NameHandler& name_handler = NameHandler::get_instance();
    std::shared_ptr<StructType> struct_type = std::make_shared<StructType>(name_handler.get_struct_type_name());
    int struct_member_count = rand_val_gen->get_rand_value(p->get_min_struct_member_count(),
                                                           p->get_max_struct_member_count());
    int member_count = 0;
    for (int i = 0; i < struct_member_count; ++i) {
        if (p->get_allow_mix_cv_qual_in_struct())
            primary_cv_qual = rand_val_gen->get_rand_elem(p->get_allowed_cv_qual());

        if (p->get_allow_mix_static_in_struct())
            primary_static_spec = p->get_allow_static_members() ? rand_val_gen->get_rand_value(false, true) : false;

        if (p->get_allow_mix_types_in_struct()) {
            Data::VarClassID member_class = rand_val_gen->get_rand_id(p->get_member_class_prob());
            bool add_substruct = false;
            std::shared_ptr<StructType> substruct_type = nullptr;
            if (member_class == Data::VarClassID::STRUCT && p->get_max_struct_depth() > 0 && nested_struct_types.size() > 0) {
                substruct_type = rand_val_gen->get_rand_elem(nested_struct_types);
                add_substruct = substruct_type->get_nest_depth() + 1 != p->get_max_struct_depth();
            }
            if (add_substruct) {
                primary_type = std::make_shared<StructType>(*substruct_type);
            }
            else {
                GenPolicy::BitFieldID bit_field_dis = rand_val_gen->get_rand_id(p->get_bit_field_prob());
                // In C, bit-field may be declared with a type other than unsigned int or signed int
                // only with "J.5.8 Extended bit-field types"
                if (options->is_c()) {
                    auto search_allowed_bit_filed_type = [] (Probability<IntegerType::IntegerTypeID> prob) {
                        return prob.get_id() == IntegerType::IntegerTypeID::INT ||
                               prob.get_id() == IntegerType::IntegerTypeID::UINT;
                    };
                    auto search_res = std::find_if(p->get_allowed_int_types().begin(),
                                                   p->get_allowed_int_types().end(),
                                                   search_allowed_bit_filed_type);
                    if (search_res == p->get_allowed_int_types().end())
                        bit_field_dis = GenPolicy::BitFieldID::MAX_BIT_FIELD_ID;
                }
                if (bit_field_dis == GenPolicy::BitFieldID::UNNAMED) {
                    struct_type->add_shadow_member(BitField::generate(ctx, true));
                    continue;
                }
                else if (bit_field_dis == GenPolicy::BitFieldID::NAMED) {
                    primary_type = BitField::generate(ctx);
                    primary_static_spec = false; // BitField can't be static member of struct
                }
                else
                    primary_type = IntegerType::generate(ctx);
            }
        }
        primary_type->set_cv_qual(primary_cv_qual);
        primary_type->set_is_static(primary_static_spec);
        // FIXME: Check for tiger syntax
        struct_type->add_member(primary_type, "member_" + std::to_string(name_handler.get_struct_type_count()) + "_" +
                                              std::to_string(member_count++));
    }
    return struct_type;
}

//TODO: maybe we can use template instead of it?
#define CAST_CASE(new_val_memb)                                     \
switch (int_type_id) {                                              \
    case Type::IntegerTypeID::BOOL:                                 \
        new_val_memb = val.bool_val;                                \
        break;                                                      \
    case Type::IntegerTypeID::CHAR:                                 \
        new_val_memb = val.char_val;                                \
        break;                                                      \
    case Type::IntegerTypeID::UCHAR:                                \
        new_val_memb = val.uchar_val;                               \
        break;                                                      \
    case Type::IntegerTypeID::SHRT:                                 \
        new_val_memb = val.shrt_val;                                \
        break;                                                      \
    case Type::IntegerTypeID::USHRT:                                \
        new_val_memb = val.ushrt_val;                               \
        break;                                                      \
    case Type::IntegerTypeID::INT:                                  \
        new_val_memb = val.int_val;                                 \
        break;                                                      \
    case Type::IntegerTypeID::UINT:                                 \
        new_val_memb = val.uint_val;                                \
        break;                                                      \
    case Type::IntegerTypeID::LINT:                                 \
        if (options->mode_64bit)                                    \
            new_val_memb = val.lint64_val;                          \
        else                                                        \
            new_val_memb = val.lint32_val;                          \
        break;                                                      \
    case Type::IntegerTypeID::ULINT:                                \
        if (options->mode_64bit)                                    \
            new_val_memb = val.ulint64_val;                         \
        else                                                        \
            new_val_memb = val.ulint32_val;                         \
        break;                                                      \
    case Type::IntegerTypeID::LLINT:                                \
        new_val_memb = val.llint_val;                               \
        break;                                                      \
    case Type::IntegerTypeID::ULLINT:                               \
        new_val_memb = val.ullint_val;                              \
        break;                                                      \
    case Type::IntegerTypeID::MAX_INT_ID:                           \
        ERROR("unsupported int type (BuiltinType::ScalarTypedVal)");\
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::cast_type (Type::IntegerTypeID to_type_id) {
    ScalarTypedVal new_val = ScalarTypedVal (to_type_id);
    switch (to_type_id) {
        case Type::IntegerTypeID::BOOL:
            CAST_CASE(new_val.val.bool_val)
            break;
        case Type::IntegerTypeID::CHAR:
            CAST_CASE(new_val.val.char_val)
            break;
        case Type::IntegerTypeID::UCHAR:
            CAST_CASE(new_val.val.uchar_val)
            break;
        case Type::IntegerTypeID::SHRT:
            CAST_CASE(new_val.val.shrt_val)
            break;
        case Type::IntegerTypeID::USHRT:
            CAST_CASE(new_val.val.ushrt_val)
            break;
        case Type::IntegerTypeID::INT:
            CAST_CASE(new_val.val.int_val)
            break;
        case Type::IntegerTypeID::UINT:
            CAST_CASE(new_val.val.uint_val)
            break;
        case Type::IntegerTypeID::LINT:
            if (options->mode_64bit)
                CAST_CASE(new_val.val.lint64_val)
            else
                CAST_CASE(new_val.val.lint32_val)
            break;
        case Type::IntegerTypeID::ULINT:
            if (options->mode_64bit)
                CAST_CASE(new_val.val.ulint64_val)
            else
                CAST_CASE(new_val.val.ulint32_val)
            break;
        case Type::IntegerTypeID::LLINT:
            CAST_CASE(new_val.val.llint_val)
            break;
        case Type::IntegerTypeID::ULLINT:
            CAST_CASE(new_val.val.ullint_val)
            break;
        case Type::IntegerTypeID::MAX_INT_ID:
            ERROR("unsupported int type (BuiltinType::ScalarTypedVal)");
    }
    return new_val;
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::pre_op (bool inc) { // Prefix
    BuiltinType::ScalarTypedVal ret = *this;
    int add = inc ? 1 : -1;
    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
            ERROR("bool is illegal in dec and inc operators (BuiltinType::ScalarTypedVal)");
        //TODO: is it UB if we pre-increment char and short?
        case IntegerType::IntegerTypeID::CHAR:
            if ((val.char_val == CHAR_MAX && add > 0) ||
                (val.char_val == CHAR_MIN && add < 0))
                ret.set_ub(UB::SignOvf);
            else
                ret.val.char_val = val.char_val + add;
            break;
        case IntegerType::IntegerTypeID::UCHAR:
            ret.val.uchar_val = val.uchar_val + add;
        case IntegerType::IntegerTypeID::SHRT:
            if ((val.shrt_val == SHRT_MAX && add > 0) ||
                (val.shrt_val == SHRT_MIN && add < 0))
                ret.set_ub(UB::SignOvf);
            else
                ret.val.shrt_val = val.shrt_val + add;
            break;
        case IntegerType::IntegerTypeID::USHRT:
            ret.val.ushrt_val = val.ushrt_val + add;
        case IntegerType::IntegerTypeID::INT:
            if ((val.int_val == INT_MAX && add > 0) ||
                (val.int_val == INT_MIN && add < 0))
                ret.set_ub(UB::SignOvf);
            else
                ret.val.int_val = val.int_val + add;
            break;
        case IntegerType::IntegerTypeID::UINT:
            ret.val.uint_val = val.uint_val + add;
            break;
        case IntegerType::IntegerTypeID::LINT:
        {
            auto lint_pre_op = [add, &ret] (auto src, auto &dest, auto min, auto max) {
                if ((src == max && add > 0) ||
                    (src == min && add < 0))
                    ret.set_ub(UB::SignOvf);
                else
                    dest = src + add;
            };
            if (options->mode_64bit)
                lint_pre_op(val.lint64_val, ret.val.lint64_val, LLONG_MIN, LLONG_MAX);
            else
                lint_pre_op(val.lint32_val, ret.val.lint32_val, INT_MIN, INT_MAX);
        }
            break;
        case IntegerType::IntegerTypeID::ULINT:
            if (options->mode_64bit)
                ret.val.ulint64_val = val.ulint64_val + add;
            else
                ret.val.ulint32_val = val.ulint32_val + add;
            break;
        case IntegerType::IntegerTypeID::LLINT:
            if ((val.llint_val == LLONG_MAX && add > 0) ||
                (val.llint_val == LLONG_MIN && add < 0))
                ret.set_ub(UB::SignOvf);
            else
                ret.val.llint_val = val.llint_val + add;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            ret.val.ullint_val = val.ullint_val + add;
            break;
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
    }
    return ret;
}

#define LINT_SINGLE_OPT(op, sign)                                    \
    if (options->mode_64bit)                                         \
        ret.val.sign##lint64_val = op val.sign##lint64_val;          \
    else                                                             \
        ret.val.sign##lint32_val = op val.sign##lint32_val;

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator- () {
    BuiltinType::ScalarTypedVal ret = *this;
    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            if (val.int_val == INT_MIN)
                ret.set_ub(UB::SignOvf);
            else
                ret.val.int_val = -val.int_val;
            break;
        case IntegerType::IntegerTypeID::UINT:
            ret.val.uint_val = -val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
        {
            auto lint_op_minus = [&ret] (auto src, auto &dest, auto min) {
                if (src == min)
                    ret.set_ub(UB::SignOvf);
                else
                    dest = -src;
            };
            if (options->mode_64bit)
                lint_op_minus(val.lint64_val, ret.val.lint64_val, LLONG_MIN);
            else
                lint_op_minus(val.lint32_val, ret.val.lint32_val, INT_MIN);
        }
            break;
        case IntegerType::IntegerTypeID::ULINT:
            LINT_SINGLE_OPT(-, u);
            break;
        case IntegerType::IntegerTypeID::LLINT:
            if (val.llint_val == LLONG_MIN)
                ret.set_ub(UB::SignOvf);
            else
                ret.val.llint_val = -val.llint_val;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            ret.val.ullint_val = -val.ullint_val;
            break;
    }
    return ret;

}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator~ () {
    BuiltinType::ScalarTypedVal ret = *this;
    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            ret.val.int_val = ~val.int_val;
            break;
        case IntegerType::IntegerTypeID::UINT:
            ret.val.uint_val = ~val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
            LINT_SINGLE_OPT(~,);
            break;
        case IntegerType::IntegerTypeID::ULINT:
            LINT_SINGLE_OPT(~, u);
            break;
        case IntegerType::IntegerTypeID::LLINT:
            ret.val.llint_val = ~val.llint_val;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            ret.val.ullint_val = ~val.ullint_val;
            break;
    }
    return ret;
}

uint64_t BuiltinType::ScalarTypedVal::get_abs_val () {
    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
            return val.bool_val;
        case IntegerType::IntegerTypeID::CHAR:
            return std::abs(val.char_val);
        case IntegerType::IntegerTypeID::UCHAR:
            return val.uchar_val;
        case IntegerType::IntegerTypeID::SHRT:
            return std::abs(val.shrt_val);
        case IntegerType::IntegerTypeID::USHRT:
            return val.ushrt_val;
        case IntegerType::IntegerTypeID::INT:
            return std::abs(val.int_val);
        case IntegerType::IntegerTypeID::UINT:
            return val.uint_val;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit)
                return std::abs(val.lint64_val);
            else
                return std::abs(val.lint32_val);
        case IntegerType::IntegerTypeID::ULINT:
            if (options->mode_64bit)
                return val.ulint64_val;
            else
                return val.ulint32_val;
        case IntegerType::IntegerTypeID::LLINT:
            return std::abs(val.llint_val);
        case IntegerType::IntegerTypeID::ULLINT:
            return val.ullint_val;
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
    }
    // TODO: add unreachable macro to the project.
    ERROR("reaching unreachable code (BuiltinType::ScalarTypedVal)");
}

void BuiltinType::ScalarTypedVal::set_abs_val (uint64_t new_val) {
    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
            val.bool_val = new_val;
            break;
        case IntegerType::IntegerTypeID::CHAR:
            val.char_val = new_val;
            break;
        case IntegerType::IntegerTypeID::UCHAR:
            val.uchar_val = new_val;
            break;
        case IntegerType::IntegerTypeID::SHRT:
            val.shrt_val = new_val;
            break;
        case IntegerType::IntegerTypeID::USHRT:
            val.ushrt_val = new_val;
            break;
        case IntegerType::IntegerTypeID::INT:
            val.int_val = new_val;
            break;
        case IntegerType::IntegerTypeID::UINT:
            val.uint_val = new_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit)
                val.lint64_val = new_val;
            else
                val.lint32_val = new_val;
            break;
        case IntegerType::IntegerTypeID::ULINT:
            if (options->mode_64bit)
                val.ulint64_val = new_val;
            else
                val.ulint32_val = new_val;
            break;
        case IntegerType::IntegerTypeID::LLINT:
            val.llint_val = new_val;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            val.ullint_val = new_val;
            break;
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
    }
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator! () {
    Type::IntegerTypeID ret_type_id = Type::IntegerTypeID::BOOL;
    if (options->is_c())
        ret_type_id = Type::IntegerTypeID::INT;
    BuiltinType::ScalarTypedVal ret = BuiltinType::ScalarTypedVal(ret_type_id);
    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
            if (options->is_cxx()) {
                ret.val.bool_val = !val.bool_val;
                break;
            }
        case IntegerType::IntegerTypeID::INT:
            if (options->is_c()) {
                ret.val.int_val = !val.int_val;
                break;
            }
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::UINT:
        case IntegerType::IntegerTypeID::LINT:
        case IntegerType::IntegerTypeID::ULINT:
        case IntegerType::IntegerTypeID::LLINT:
        case IntegerType::IntegerTypeID::ULLINT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
    }
    return ret;
}

#define LINT_DOUBLE_OPT(op, sign)                                                    \
    if (options->mode_64bit)                                                         \
        ret.val.sign##lint64_val = val.sign##lint64_val op rhs.val.sign##lint64_val; \
    else                                                                             \
        ret.val.sign##lint32_val = val.sign##lint32_val op rhs.val.sign##lint32_val;

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator+ (ScalarTypedVal rhs) {
    BuiltinType::ScalarTypedVal ret = *this;

    int64_t s_tmp = 0;
    uint64_t u_tmp = 0;

    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            s_tmp = (long long int) val.int_val + (long long int) rhs.val.int_val;
            if (s_tmp < INT_MIN || s_tmp > INT_MAX)
                ret.set_ub(SignOvf);
            else
                ret.val.int_val = (int) s_tmp;
            break;
        case IntegerType::IntegerTypeID::UINT:
            ret.val.uint_val = val.uint_val + rhs.val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit) {
                uint64_t ua = val.lint64_val;
                uint64_t ub = rhs.val.lint64_val;
                u_tmp = ua + ub;
                ua = (ua >> 63) + LLONG_MAX;
                if ((int64_t) ((ua ^ ub) | ~(ub ^ u_tmp)) >= 0)
                    ret.set_ub(SignOvf);
                else
                    ret.val.lint64_val = (long long int) u_tmp;
            }
            else {
                s_tmp = (long long int) val.lint32_val + (long long int) rhs.val.lint32_val;
                if (s_tmp < INT_MIN || s_tmp > INT_MAX)
                    ret.set_ub(SignOvf);
                else
                    ret.val.lint32_val = (int) s_tmp;
            }
            break;
        case IntegerType::IntegerTypeID::ULINT:
            LINT_DOUBLE_OPT(+, u);
            break;
        case IntegerType::IntegerTypeID::LLINT:
        {
            uint64_t ua = val.llint_val;
            uint64_t ub = rhs.val.llint_val;
            u_tmp = ua + ub;
            ua = (ua >> 63) + LLONG_MAX;
            if ((int64_t) ((ua ^ ub) | ~(ub ^ u_tmp)) >= 0)
                ret.set_ub(SignOvf);
            else
                ret.val.llint_val =  val.llint_val + rhs.val.llint_val;
            break;
        }
        case IntegerType::IntegerTypeID::ULLINT:
            ret.val.ullint_val = val.ullint_val + rhs.val.ullint_val;
            break;
    }
    return ret;
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator- (ScalarTypedVal rhs) {
    BuiltinType::ScalarTypedVal ret = *this;

    int64_t s_tmp = 0;
    uint64_t u_tmp = 0;

    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            s_tmp = (long long int) val.int_val - (long long int) rhs.val.int_val;
            if (s_tmp < INT_MIN || s_tmp > INT_MAX)
                ret.set_ub(SignOvf);
            else
                ret.val.int_val = (int) s_tmp;
            break;
        case IntegerType::IntegerTypeID::UINT:
            ret.val.uint_val = val.uint_val - rhs.val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit) {
                uint64_t ua = val.lint64_val;
                uint64_t ub = rhs.val.lint64_val;
                u_tmp = ua - ub;
                ua = (ua >> 63) + LLONG_MAX;
                if ((int64_t) ((ua ^ ub) & (ua ^ u_tmp)) < 0)
                    ret.set_ub(SignOvf);
                else
                    ret.val.lint64_val = (long long int) u_tmp;
            }
            else {
                s_tmp = (long long int) val.lint32_val - (long long int) rhs.val.lint32_val;
                if (s_tmp < INT_MIN || s_tmp > INT_MAX)
                    ret.set_ub(SignOvf);
                else
                    ret.val.lint32_val = (int) s_tmp;
            }
            break;
        case IntegerType::IntegerTypeID::ULINT:
            LINT_DOUBLE_OPT(-, u);
            break;
        case IntegerType::IntegerTypeID::LLINT:
        {
            uint64_t ua = val.llint_val;
            uint64_t ub = rhs.val.llint_val;
            u_tmp = ua - ub;
            ua = (ua >> 63) + LLONG_MAX;
            if ((int64_t) ((ua ^ ub) & (ua ^ u_tmp)) < 0)
                ret.set_ub(SignOvf);
            else
                ret.val.llint_val = (long long int) u_tmp;
            break;
        }
        case IntegerType::IntegerTypeID::ULLINT:
            ret.val.ullint_val = val.ullint_val - rhs.val.ullint_val;
            break;
    }
    return ret;
}

static bool check_int64_mul (int64_t a, int64_t b, int64_t* res) {
    uint64_t ret = 0;

    int8_t sign = (((a > 0) && (b > 0)) || ((a < 0) && (b < 0))) ? 1 : -1;
    uint64_t a_abs = 0;
    uint64_t b_abs = 0;

    if (a == INT64_MIN)
        // Operation "-" is undefined for "INT64_MIN", as it causes overflow.
        // But converting INT64_MIN to unsigned type yields the correct result,
        // i.e. it will be positive value -INT64_MIN.
        // See 6.3.1.3 section in C99 standart for more details
        a_abs = (uint64_t) INT64_MIN;
    else
        a_abs = (a > 0) ? a : -a;

    if (b == INT64_MIN)
        b_abs = (uint64_t) INT64_MIN;
    else
        b_abs = (b > 0) ? b : -b;

    uint32_t a0 = a_abs & 0xFFFFFFFF;
    uint32_t b0 = b_abs & 0xFFFFFFFF;
    uint32_t a1 = a_abs >> 32;
    uint32_t b1 = b_abs >> 32;

    if ((a1 != 0) && (b1 != 0))
        return false;

    uint64_t tmp = (((uint64_t) a1) * b0) + (((uint64_t) b1) * a0);
    if (tmp > 0xFFFFFFFF)
        return false;

    ret = (tmp << 32) + (((uint64_t) a0) * b0);
    if (ret < (tmp << 32))
        return false;

    if ((sign < 0) && (ret > (uint64_t) INT64_MIN)) {
        return false;
    } else if ((sign > 0) && (ret > INT64_MAX)) {
        return false;
    } else {
        *res = ret * sign;
    }
    return true;
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator* (ScalarTypedVal rhs) {
    BuiltinType::ScalarTypedVal ret = *this;

    int64_t s_tmp = 0;

    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            s_tmp = (long long int) val.int_val * (long long int) rhs.val.int_val;
            if ((int) val.int_val == INT_MIN && (int) rhs.val.int_val == -1)
                ret.set_ub(SignOvfMin);
            else if (s_tmp < INT_MIN || s_tmp > INT_MAX)
                ret.set_ub(SignOvf);
            else
                ret.val.int_val = (int) s_tmp;
            break;
        case IntegerType::IntegerTypeID::UINT:
            ret.val.uint_val = val.uint_val * rhs.val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit) {
                if (!check_int64_mul(val.lint64_val, rhs.val.lint64_val, &s_tmp))
                    ret.set_ub(SignOvf);
                else
                    ret.val.lint64_val = (long long int) s_tmp;
            }
            else {
                s_tmp = (long long int) val.lint32_val * (long long int) rhs.val.lint32_val;
                if (s_tmp < INT_MIN || s_tmp > INT_MAX)
                    ret.set_ub(SignOvf);
                else
                    ret.val.lint32_val = (int) s_tmp;
            }
            break;
        case IntegerType::IntegerTypeID::ULINT:
            LINT_DOUBLE_OPT(*, u);
            break;
        case IntegerType::IntegerTypeID::LLINT:
            if ((long long int) val.llint_val == LLONG_MIN && (long long int) rhs.val.llint_val == -1)
                ret.set_ub(SignOvfMin);
            else if (!check_int64_mul(val.llint_val, rhs.val.llint_val, &s_tmp))
                ret.set_ub(SignOvfMin);
            else
                ret.val.llint_val = (long long int) s_tmp;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            ret.val.ullint_val = val.ullint_val * rhs.val.ullint_val;
            break;
    }
    return ret;
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator/ (ScalarTypedVal rhs) {
    BuiltinType::ScalarTypedVal ret = *this;

    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            if (rhs.val.int_val == 0) {
                ret.set_ub(ZeroDiv);
                return ret;
            }
            if ((val.int_val == INT_MIN && rhs.val.int_val == -1) ||
                (rhs.val.int_val == INT_MIN && val.int_val == -1))
                ret.set_ub(SignOvf);
            else
                ret.val.int_val = val.int_val / rhs.val.int_val;
            break;
        case IntegerType::IntegerTypeID::UINT:
            if (rhs.val.uint_val == 0) {
                ret.set_ub(ZeroDiv);
                return ret;
            }
            ret.val.uint_val = val.uint_val / rhs.val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
        {
            auto lint_op_div = [&ret] (auto lhs, auto rhs, auto &dest, auto min) {
                if (rhs == 0)
                    ret.set_ub(ZeroDiv);
                else if ((lhs == min && rhs == -1) ||
                         (rhs == min && lhs == -1))
                    ret.set_ub(SignOvf);
                else
                    dest = lhs / rhs;
            };
            if (options->mode_64bit)
                lint_op_div (val.lint64_val, rhs.val.lint64_val, ret.val.lint64_val, LLONG_MIN);
            else
                lint_op_div (val.lint32_val, rhs.val.lint32_val, ret.val.lint32_val, INT_MIN);
        }
            break;
        case IntegerType::IntegerTypeID::ULINT:
        {
            auto ulint_op_div = [&ret] (auto lhs, auto rhs, auto &dest) {
                if (rhs == 0)
                    ret.set_ub(ZeroDiv);
                else
                    dest = lhs / rhs;
            };
            if (options->mode_64bit)
                ulint_op_div(val.ulint64_val, rhs.val.ulint64_val, ret.val.ulint64_val);
            else
                ulint_op_div(val.ulint32_val, rhs.val.ulint32_val, ret.val.ulint32_val);
        }
            break;
        case IntegerType::IntegerTypeID::LLINT:
            if (rhs.val.llint_val == 0) {
                ret.set_ub(ZeroDiv);
                return ret;
            }
            if ((val.llint_val == LLONG_MIN && rhs.val.llint_val == -1) ||
                (rhs.val.llint_val == LLONG_MIN && val.llint_val == -1))
                ret.set_ub(SignOvf);
            else
                ret.val.llint_val = val.llint_val / rhs.val.llint_val;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            if (rhs.val.ullint_val == 0) {
                ret.set_ub(ZeroDiv);
                return ret;
            }
            ret.val.ullint_val = val.ullint_val / rhs.val.ullint_val;
            break;
    }
    return ret;
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator% (ScalarTypedVal rhs) {
    BuiltinType::ScalarTypedVal ret = *this;

    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            if (rhs.val.int_val == 0) {
                ret.set_ub(ZeroDiv);
                return ret;
            }
            if ((val.int_val == INT_MIN && rhs.val.int_val == -1) ||
                (rhs.val.int_val == INT_MIN && val.int_val == -1))
                ret.set_ub(SignOvf);
            else
                ret.val.int_val = val.int_val % rhs.val.int_val;
            break;
        case IntegerType::IntegerTypeID::UINT:
            if (rhs.val.uint_val == 0) {
                ret.set_ub(ZeroDiv);
                return ret;
            }
            ret.val.uint_val = val.uint_val % rhs.val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT: {
            auto lint_op_mod = [&ret](auto lhs, auto rhs, auto &dest, auto min) {
                if (rhs == 0)
                    ret.set_ub(ZeroDiv);
                else if ((lhs == min && rhs == -1) ||
                         (rhs == min && lhs == -1))
                    ret.set_ub(SignOvf);
                else
                    dest = lhs % rhs;
            };
            if (options->mode_64bit)
                lint_op_mod(val.lint64_val, rhs.val.lint64_val, ret.val.lint64_val, LLONG_MIN);
            else
                lint_op_mod(val.lint32_val, rhs.val.lint32_val, ret.val.lint32_val, INT_MIN);
        }
            break;
        case IntegerType::IntegerTypeID::ULINT:
        {
            auto ulint_op_mod = [&ret] (auto lhs, auto rhs, auto &dest) {
                if (rhs == 0)
                    ret.set_ub(ZeroDiv);
                else
                    dest = lhs % rhs;
            };
            if (options->mode_64bit)
                ulint_op_mod(val.ulint64_val, rhs.val.ulint64_val, ret.val.ulint64_val);
            else
                ulint_op_mod(val.ulint32_val, rhs.val.ulint32_val, ret.val.ulint32_val);
        }
            break;
        case IntegerType::IntegerTypeID::LLINT:
            if (rhs.val.llint_val == 0) {
                ret.set_ub(ZeroDiv);
                return ret;
            }
            if ((val.llint_val == LLONG_MIN && rhs.val.llint_val == -1) ||
                (rhs.val.llint_val == LLONG_MIN && val.llint_val == -1))
                ret.set_ub(SignOvf);
            else
                ret.val.llint_val = val.llint_val % rhs.val.llint_val;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            if (rhs.val.ullint_val == 0) {
                ret.set_ub(ZeroDiv);
                return ret;
            }
            ret.val.ullint_val = val.ullint_val % rhs.val.ullint_val;
            break;
    }
    return ret;
}

#define ScalarTypedValCmpOp(__op__)                                                                 \
BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator __op__ (ScalarTypedVal rhs) {     \
    BuiltinType::ScalarTypedVal ret = BuiltinType::ScalarTypedVal(Type::IntegerTypeID::BOOL);       \
                                                                                                    \
    switch (int_type_id) {                                                                          \
        case IntegerType::IntegerTypeID::BOOL:                                                      \
            ret.val.bool_val = val.bool_val __op__ rhs.val.bool_val;                                \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::CHAR:                                                      \
            ret.val.bool_val = val.char_val __op__ rhs.val.char_val;                                \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::UCHAR:                                                     \
            ret.val.bool_val = val.uchar_val __op__ rhs.val.uchar_val;                              \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::SHRT:                                                      \
            ret.val.bool_val = val.shrt_val __op__ rhs.val.shrt_val;                                \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::USHRT:                                                     \
            ret.val.bool_val = val.ushrt_val __op__ rhs.val.ushrt_val;                              \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::INT:                                                       \
            ret.val.bool_val = val.int_val __op__ rhs.val.int_val;                                  \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::UINT:                                                      \
            ret.val.bool_val = val.uint_val __op__ rhs.val.uint_val;                                \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::LINT:                                                      \
            if (options->mode_64bit)                                                                \
                ret.val.bool_val = val.lint64_val __op__ rhs.val.lint64_val;                        \
            else                                                                                    \
                ret.val.bool_val = val.lint32_val __op__ rhs.val.lint32_val;                        \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::ULINT:                                                     \
            if (options->mode_64bit)                                                                \
                ret.val.bool_val = val.ulint64_val __op__ rhs.val.ulint64_val;                      \
            else                                                                                    \
                ret.val.bool_val = val.ulint32_val __op__ rhs.val.ulint32_val;                      \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::LLINT:                                                     \
            ret.val.bool_val = val.llint_val __op__ rhs.val.llint_val;                              \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::ULLINT:                                                    \
            ret.val.bool_val = val.ullint_val __op__ rhs.val.ullint_val;                            \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::MAX_INT_ID:                                                \
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");                          \
    }                                                                                               \
    return ret;                                                                                     \
}

ScalarTypedValCmpOp(<)
ScalarTypedValCmpOp(>)
ScalarTypedValCmpOp(<=)
ScalarTypedValCmpOp(>=)
ScalarTypedValCmpOp(==)
ScalarTypedValCmpOp(!=)

#define ScalarTypedValLogOp(__op__)                                                                 \
BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator __op__ (ScalarTypedVal rhs) {     \
    Type::IntegerTypeID ret_type_id = Type::IntegerTypeID::BOOL;                                    \
    if (options->is_c())                                                                            \
        ret_type_id = Type::IntegerTypeID::INT;                                                     \
    BuiltinType::ScalarTypedVal ret = BuiltinType::ScalarTypedVal(ret_type_id);                     \
                                                                                                    \
    switch (int_type_id) {                                                                          \
        case IntegerType::IntegerTypeID::BOOL:                                                      \
            if (options->is_cxx()) {                                                                \
                ret.val.bool_val = val.bool_val __op__ rhs.val.bool_val;                            \
                break;                                                                              \
            }                                                                                       \
        case IntegerType::IntegerTypeID::INT:                                                       \
            if (options->is_c()) {                                                                  \
                ret.val.int_val = val.int_val __op__ rhs.val.int_val;                               \
                break;                                                                              \
            }                                                                                       \
        case IntegerType::IntegerTypeID::CHAR:                                                      \
        case IntegerType::IntegerTypeID::UCHAR:                                                     \
        case IntegerType::IntegerTypeID::SHRT:                                                      \
        case IntegerType::IntegerTypeID::USHRT:                                                     \
        case IntegerType::IntegerTypeID::UINT:                                                      \
        case IntegerType::IntegerTypeID::LINT:                                                      \
        case IntegerType::IntegerTypeID::ULINT:                                                     \
        case IntegerType::IntegerTypeID::LLINT:                                                     \
        case IntegerType::IntegerTypeID::ULLINT:                                                    \
        case IntegerType::IntegerTypeID::MAX_INT_ID:                                                \
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");                          \
    }                                                                                               \
    return ret;                                                                                     \
}

ScalarTypedValLogOp(&&)

ScalarTypedValLogOp(||)

#define ScalarTypedValBitOp(__op__)                                                                 \
BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator __op__ (ScalarTypedVal rhs) {     \
    BuiltinType::ScalarTypedVal ret = *this;                                                        \
                                                                                                    \
    switch (int_type_id) {                                                                          \
        case IntegerType::IntegerTypeID::BOOL:                                                      \
        case IntegerType::IntegerTypeID::CHAR:                                                      \
        case IntegerType::IntegerTypeID::UCHAR:                                                     \
        case IntegerType::IntegerTypeID::SHRT:                                                      \
        case IntegerType::IntegerTypeID::USHRT:                                                     \
        case IntegerType::IntegerTypeID::MAX_INT_ID:                                                \
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");                          \
        case IntegerType::IntegerTypeID::INT:                                                       \
            ret.val.int_val = val.int_val __op__ rhs.val.int_val;                                   \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::UINT:                                                      \
            ret.val.uint_val = val.uint_val __op__ rhs.val.uint_val;                                \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::LINT:                                                      \
            if (options->mode_64bit)                                                                \
                ret.val.lint64_val = val.lint64_val __op__ rhs.val.lint64_val;                      \
            else                                                                                    \
                ret.val.lint32_val = val.lint32_val __op__ rhs.val.lint32_val;                      \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::ULINT:                                                     \
            if (options->mode_64bit)                                                                \
                ret.val.ulint64_val = val.ulint64_val __op__ rhs.val.ulint64_val;                   \
            else                                                                                    \
                ret.val.ulint32_val = val.ulint32_val __op__ rhs.val.ulint32_val;                   \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::LLINT:                                                     \
            ret.val.llint_val = val.llint_val __op__ rhs.val.llint_val;                             \
            break;                                                                                  \
        case IntegerType::IntegerTypeID::ULLINT:                                                    \
            ret.val.ullint_val = val.ullint_val __op__ rhs.val.ullint_val;                          \
            break;                                                                                  \
    }                                                                                               \
    return ret;                                                                                     \
}

ScalarTypedValBitOp(&)
ScalarTypedValBitOp(|)
ScalarTypedValBitOp(^)

#define SHFT_CASE(__op__, ret_val, lhs_val)                                     \
switch (rhs.get_int_type_id()) {                                                \
    case IntegerType::IntegerTypeID::BOOL:                                      \
    case IntegerType::IntegerTypeID::CHAR:                                      \
    case IntegerType::IntegerTypeID::UCHAR:                                     \
    case IntegerType::IntegerTypeID::SHRT:                                      \
    case IntegerType::IntegerTypeID::USHRT:                                     \
    case IntegerType::IntegerTypeID::MAX_INT_ID:                                \
        ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");          \
    case IntegerType::IntegerTypeID::INT:                                       \
        ret_val = lhs_val __op__ rhs.val.int_val;                               \
        break;                                                                  \
    case IntegerType::IntegerTypeID::UINT:                                      \
        ret_val = lhs_val __op__ rhs.val.uint_val;                              \
        break;                                                                  \
    case IntegerType::IntegerTypeID::LINT:                                      \
        if (options->mode_64bit)                                                \
            ret_val = lhs_val __op__ rhs.val.lint64_val;                        \
        else                                                                    \
            ret_val = lhs_val __op__ rhs.val.lint32_val;                        \
        break;                                                                  \
    case IntegerType::IntegerTypeID::ULINT:                                     \
        if (options->mode_64bit)                                                \
            ret_val = lhs_val __op__ rhs.val.ulint64_val;                       \
        else                                                                    \
            ret_val = lhs_val __op__ rhs.val.ulint32_val;                       \
        break;                                                                  \
    case IntegerType::IntegerTypeID::LLINT:                                     \
        ret_val = lhs_val __op__ rhs.val.llint_val;                             \
        break;                                                                  \
    case IntegerType::IntegerTypeID::ULLINT:                                    \
        ret_val = lhs_val __op__ rhs.val.ullint_val;                            \
        break;                                                                  \
}

static uint32_t msb(uint64_t x) {
    uint32_t ret = 0;
    while (x != 0) {
        ret++;
        x = x >> 1;
    }
    return ret;
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator<< (ScalarTypedVal rhs) {
    BuiltinType::ScalarTypedVal ret = *this;

    int64_t s_lhs = 0;
//    uint64_t u_lhs = 0;
    int64_t s_rhs = 0;
    uint64_t u_rhs = 0;
    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            s_lhs = val.int_val;
            break;
        case IntegerType::IntegerTypeID::UINT:
//            u_lhs = val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit)
                s_lhs = val.lint64_val;
            else
                s_lhs = val.lint32_val;
            break;
        case IntegerType::IntegerTypeID::ULINT:
//            if (options->mode_64bit)
//                u_lhs = val.ulint64_val;
//            else
//                u_lhs = val.ulint32_val;
            break;
        case IntegerType::IntegerTypeID::LLINT:
            s_lhs = val.llint_val;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
//            u_lhs = val.ullint_val;
            break;
    }

    switch (rhs.get_int_type_id()) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            s_rhs = rhs.val.int_val;
            break;
        case IntegerType::IntegerTypeID::UINT:
            u_rhs = rhs.val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit)
                s_rhs = rhs.val.lint64_val;
            else
                s_rhs = rhs.val.lint32_val;
            break;
        case IntegerType::IntegerTypeID::ULINT:
            if (options->mode_64bit)
                u_rhs = rhs.val.ulint64_val;
            else
                u_rhs = rhs.val.ulint32_val;
            break;
        case IntegerType::IntegerTypeID::LLINT:
            s_rhs = rhs.val.llint_val;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            u_rhs = rhs.val.ullint_val;
            break;
    }

    bool lhs_is_signed = IntegerType::init(int_type_id)->get_is_signed();
    bool rhs_is_signed = IntegerType::init(rhs.get_int_type_id())->get_is_signed();
    if (lhs_is_signed && (s_lhs < 0)) {
        ret.set_ub(NegShift);
        return ret;
    }
    if (rhs_is_signed && (s_rhs < 0)) {
        ret.set_ub(ShiftRhsNeg);
        return ret;
    }

    uint32_t lhs_bit_size = IntegerType::init(int_type_id)->get_bit_size();
    if (rhs_is_signed) {
        if (s_rhs >= (int)lhs_bit_size) {
            ret.set_ub(ShiftRhsLarge);
            return ret;
        }
    }
    else {
        if (u_rhs >= lhs_bit_size) {
            ret.set_ub(ShiftRhsLarge);
            return ret;
        }
    }

    if (lhs_is_signed) {
        uint32_t max_avail_shft = lhs_bit_size - msb(s_lhs);
        if (rhs_is_signed && s_rhs >= (int)max_avail_shft) {
            ret.set_ub(ShiftRhsLarge);
            return ret;
        }
        else if (!rhs_is_signed && u_rhs >= max_avail_shft) {
            ret.set_ub(ShiftRhsLarge);
            return ret;
        }
    }

    if (ret.has_ub())
        return ret;

    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            SHFT_CASE(<<, ret.val.int_val, val.int_val)
            break;
        case IntegerType::IntegerTypeID::UINT:
            SHFT_CASE(<<, ret.val.uint_val, val.uint_val)
            break;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit)
                SHFT_CASE(<<, ret.val.lint64_val, val.lint64_val)
            else
                SHFT_CASE(<<, ret.val.lint32_val, val.lint32_val)
            break;
        case IntegerType::IntegerTypeID::ULINT:
            if (options->mode_64bit)
                SHFT_CASE(<<, ret.val.ulint64_val, val.ulint64_val)
            else
                SHFT_CASE(<<, ret.val.ulint32_val, val.ulint32_val)
            break;
        case IntegerType::IntegerTypeID::LLINT:
            SHFT_CASE(<<, ret.val.llint_val, val.llint_val)
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            SHFT_CASE(<<, ret.val.ullint_val, val.ullint_val)
            break;
    }
    return ret;
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::operator>> (ScalarTypedVal rhs) {
    BuiltinType::ScalarTypedVal ret = *this;

    int64_t s_lhs = 0;
//    uint64_t u_lhs = 0;
    int64_t s_rhs = 0;
    uint64_t u_rhs = 0;
    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            s_lhs = val.int_val;
            break;
        case IntegerType::IntegerTypeID::UINT:
//            u_lhs = val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit)
                s_lhs = val.lint64_val;
            else
                s_lhs = val.lint32_val;
            break;
        case IntegerType::IntegerTypeID::ULINT:
//            if (options->mode_64bit)
//                u_lhs = val.ulint64_val;
//            else
//                u_lhs = val.ulint32_val;
            break;
        case IntegerType::IntegerTypeID::LLINT:
            s_lhs = val.llint_val;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
//            u_lhs = val.ullint_val;
            break;
    }

    switch (rhs.get_int_type_id()) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            s_rhs = rhs.val.int_val;
            break;
        case IntegerType::IntegerTypeID::UINT:
            u_rhs = rhs.val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit)
                s_rhs = rhs.val.lint64_val;
            else
                s_rhs = rhs.val.lint32_val;
            break;
        case IntegerType::IntegerTypeID::ULINT:
            if (options->mode_64bit)
                u_rhs = rhs.val.ulint64_val;
            else
                u_rhs = rhs.val.ulint32_val;
            break;
        case IntegerType::IntegerTypeID::LLINT:
            s_rhs = rhs.val.llint_val;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            u_rhs = rhs.val.ullint_val;
            break;
    }

    bool lhs_is_signed = IntegerType::init(int_type_id)->get_is_signed();
    bool rhs_is_signed = IntegerType::init(rhs.get_int_type_id())->get_is_signed();
    if (lhs_is_signed && (s_lhs < 0)) {
        ret.set_ub(NegShift);
        return ret;
    }
    if (rhs_is_signed && (s_rhs < 0)) {
        ret.set_ub(ShiftRhsNeg);
        return ret;
    }

    uint32_t lhs_bit_size = IntegerType::init(int_type_id)->get_bit_size();
    if (rhs_is_signed) {
        if (s_rhs >= (int)lhs_bit_size) {
            ret.set_ub(ShiftRhsLarge);
            return ret;
        }
    }
    else {
        if(u_rhs >= lhs_bit_size) {
            ret.set_ub(ShiftRhsLarge);
            return ret;
        }
    }

    if (ret.has_ub())
        return ret;

    switch (int_type_id) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("perform propagate_type (BuiltinType::ScalarTypedVal)");
        case IntegerType::IntegerTypeID::INT:
            SHFT_CASE(>>, ret.val.int_val, val.int_val)
            break;
        case IntegerType::IntegerTypeID::UINT:
            SHFT_CASE(>>, ret.val.uint_val, val.uint_val)
            break;
        case IntegerType::IntegerTypeID::LINT:
            if (options->mode_64bit)
                SHFT_CASE(>>, ret.val.lint64_val, val.lint64_val)
            else
                SHFT_CASE(>>, ret.val.lint32_val, val.lint32_val)
            break;
        case IntegerType::IntegerTypeID::ULINT:
            if (options->mode_64bit)
                SHFT_CASE(>>, ret.val.ulint64_val, val.ulint64_val)
            else
                SHFT_CASE(>>, ret.val.ulint32_val, val.ulint32_val)
            break;
        case IntegerType::IntegerTypeID::LLINT:
            SHFT_CASE(>>, ret.val.llint_val, val.llint_val)
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            SHFT_CASE(>>, ret.val.ullint_val, val.ullint_val)
            break;
    }
    return ret;
}

template <typename T>
static void gen_rand_typed_val (T& ret, T& min, T& max) {
    ret = (T) rand_val_gen->get_rand_value<T>(min, max);
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::generate (std::shared_ptr<Context> ctx, BuiltinType::IntegerTypeID _int_type_id) {
    std::shared_ptr<IntegerType> tmp_type = IntegerType::init (_int_type_id);
    BuiltinType::ScalarTypedVal min = tmp_type->get_min();
    BuiltinType::ScalarTypedVal max = tmp_type->get_max();
    return generate(ctx, min, max);
}

BuiltinType::ScalarTypedVal BuiltinType::ScalarTypedVal::generate (std::shared_ptr<Context> ctx, BuiltinType::ScalarTypedVal min, BuiltinType::ScalarTypedVal max) {
    if (min.get_int_type_id() != max.get_int_type_id()) {
        ERROR("int type of min and int type of max are different (BuiltinType::ScalarTypedVal)");
    }
    BuiltinType::ScalarTypedVal ret(min.get_int_type_id());
    switch(min.get_int_type_id()) {
        case BuiltinType::BOOL:
            gen_rand_typed_val(ret.val.bool_val, min.val.bool_val, max.val.bool_val);
            break;
        case BuiltinType::CHAR:
            gen_rand_typed_val(ret.val.char_val, min.val.char_val, max.val.char_val);
            break;
        case BuiltinType::UCHAR:
            gen_rand_typed_val(ret.val.uchar_val, min.val.uchar_val, max.val.uchar_val);
            break;
        case BuiltinType::SHRT:
            gen_rand_typed_val(ret.val.shrt_val, min.val.shrt_val, max.val.shrt_val);
            break;
        case BuiltinType::USHRT:
            gen_rand_typed_val(ret.val.ushrt_val, min.val.ushrt_val, max.val.ushrt_val);
            break;
        case BuiltinType::INT:
            gen_rand_typed_val(ret.val.int_val, min.val.int_val, max.val.int_val);
            break;
        case BuiltinType::UINT:
            gen_rand_typed_val(ret.val.uint_val, min.val.uint_val, max.val.uint_val);
            break;
        case BuiltinType::LINT:
            if (options->mode_64bit)
                gen_rand_typed_val(ret.val.lint64_val, min.val.lint64_val, max.val.lint64_val);
            else
                gen_rand_typed_val(ret.val.lint32_val, min.val.lint32_val, max.val.lint32_val);
            break;
        case BuiltinType::ULINT:
            if (options->mode_64bit)
                gen_rand_typed_val(ret.val.ulint64_val, min.val.ulint64_val, max.val.ulint64_val);
            else
                gen_rand_typed_val(ret.val.ulint32_val, min.val.ulint32_val, max.val.ulint32_val);
            break;
        case BuiltinType::LLINT:
            gen_rand_typed_val(ret.val.llint_val, min.val.llint_val, max.val.llint_val);
            break;
        case BuiltinType::ULLINT:
            gen_rand_typed_val(ret.val.ullint_val, min.val.ullint_val, max.val.ullint_val);
            break;
        case BuiltinType::MAX_INT_ID:
            ERROR("unsupported type of struct member (BuiltinType::ScalarTypedVal)");
            break;
    }
    return ret;
}

std::ostream& yarpgen::operator<< (std::ostream &out, const BuiltinType::ScalarTypedVal &scalar_typed_val) {
    switch(scalar_typed_val.get_int_type_id()) {
        case BuiltinType::BOOL:
            out << scalar_typed_val.val.bool_val;
            break;
        case BuiltinType::CHAR:
            out << scalar_typed_val.val.char_val;
            break;
        case BuiltinType::UCHAR:
            out << scalar_typed_val.val.uchar_val;
            break;
        case BuiltinType::SHRT:
            out << scalar_typed_val.val.shrt_val;
            break;
        case BuiltinType::USHRT:
            out << scalar_typed_val.val.ushrt_val;
            break;
        case BuiltinType::INT:
            out << scalar_typed_val.val.int_val;
            break;
        case BuiltinType::UINT:
            out << scalar_typed_val.val.uint_val;
            break;
        case BuiltinType::LINT:
            if (options->mode_64bit)
                out << scalar_typed_val.val.lint64_val;
            else
                out << scalar_typed_val.val.lint32_val;
            break;
        case BuiltinType::ULINT:
            if (options->mode_64bit)
                out << scalar_typed_val.val.ulint64_val;
            else
                out << scalar_typed_val.val.ulint32_val;
            break;
        case BuiltinType::LLINT:
            out << scalar_typed_val.val.llint_val;
            break;
        case BuiltinType::ULLINT:
            out << scalar_typed_val.val.ullint_val;
            break;
        case BuiltinType::MAX_INT_ID:
            ERROR("unsupported type of struct member");
            break;
    }
    return out;
}

std::shared_ptr<IntegerType> IntegerType::init (BuiltinType::IntegerTypeID _type_id) {
    std::shared_ptr<IntegerType> ret (nullptr);
    switch (_type_id) {
        case BuiltinType::IntegerTypeID::BOOL:
            ret = std::make_shared<TypeBOOL> (TypeBOOL());
            break;
        case BuiltinType::IntegerTypeID::CHAR:
            ret = std::make_shared<TypeCHAR> (TypeCHAR());
            break;
        case BuiltinType::IntegerTypeID::UCHAR:
            ret = std::make_shared<TypeUCHAR> (TypeUCHAR());
            break;
        case BuiltinType::IntegerTypeID::SHRT:
            ret = std::make_shared<TypeSHRT> (TypeSHRT());
            break;
        case BuiltinType::IntegerTypeID::USHRT:
            ret = std::make_shared<TypeUSHRT> (TypeUSHRT());
            break;
        case BuiltinType::IntegerTypeID::INT:
            ret = std::make_shared<TypeINT> (TypeINT());
            break;
        case BuiltinType::IntegerTypeID::UINT:
            ret = std::make_shared<TypeUINT> (TypeUINT());
            break;
        case BuiltinType::IntegerTypeID::LINT:
            ret = std::make_shared<TypeLINT> (TypeLINT());
            break;
        case BuiltinType::IntegerTypeID::ULINT:
            ret = std::make_shared<TypeULINT> (TypeULINT());
            break;
         case BuiltinType::IntegerTypeID::LLINT:
            ret = std::make_shared<TypeLLINT> (TypeLLINT());
            break;
         case BuiltinType::IntegerTypeID::ULLINT:
            ret = std::make_shared<TypeULLINT> (TypeULLINT());
            break;
        case MAX_INT_ID:
            break;
    }
    return ret;
}

std::shared_ptr<IntegerType> IntegerType::init (BuiltinType::IntegerTypeID _type_id, Type::CV_Qual _cv_qual, bool _is_static, uint32_t _align) {
    std::shared_ptr<IntegerType> ret = IntegerType::init (_type_id);
    ret->set_cv_qual(_cv_qual);
    ret->set_is_static(_is_static);
    ret->set_align(_align);
    return ret;
}

std::shared_ptr<IntegerType> IntegerType::generate (std::shared_ptr<Context> ctx) {
    Type::CV_Qual cv_qual = rand_val_gen->get_rand_elem(ctx->get_gen_policy()->get_allowed_cv_qual());

    bool specifier = false;
    if (ctx->get_gen_policy()->get_allow_static_var())
        specifier = rand_val_gen->get_rand_value(false, true);
    //TODO: what about align?

    IntegerType::IntegerTypeID int_type_id = rand_val_gen->get_rand_id(ctx->get_gen_policy()->get_allowed_int_types());
    return IntegerType::init(int_type_id, cv_qual, specifier, 0);
}


bool IntegerType::can_repr_value (BuiltinType::IntegerTypeID a, BuiltinType::IntegerTypeID b) {
    // This function is used for different conversion rules, so it can be called only after integral promotion
    std::shared_ptr<IntegerType> B = std::static_pointer_cast<IntegerType>(init(b));
    switch (a) {
        case INT:
            return B->get_is_signed();
        case UINT:
            if (B->get_int_type_id() == INT)
                return false;
            if (B->get_int_type_id() == LINT)
                return options->mode_64bit;
            return true;
        case LINT:
            if (!B->get_is_signed())
                return false;
            if (B->get_int_type_id() == INT)
                return !options->mode_64bit;
            return true;
        case ULINT:
            switch (B->get_int_type_id()) {
                case INT:
                    return false;
                case UINT:
                    return !options->mode_64bit;
                case LINT:
                    return false;
                case ULINT:
                    return true;
                case LLINT:
                    return !options->mode_64bit;
                case ULLINT:
                    return true;
                default:
                    ERROR("ULINT");
            }
        case LLINT:
            switch (B->get_int_type_id()) {
                case INT:
                case UINT:
                    return false;
                case LINT:
                    return options->mode_64bit;
                case ULINT:
                    return false;
                case LLINT:
                    return true;
                case ULLINT:
                    return false;
                default:
                    ERROR("LLINT");
            }
        case ULLINT:
            switch (B->get_int_type_id()) {
                case INT:
                case UINT:
                case LINT:
                   return false;
                case ULINT:
                   return options->mode_64bit;
                case LLINT:
                   return false;
                case ULLINT:
                    return true;
                default:
                    ERROR("ULLINT");
            }
        default:
            ERROR("Some types are not covered (IntegerType)");
            return false;
    }
}

BuiltinType::IntegerTypeID IntegerType::get_corr_unsig (BuiltinType::IntegerTypeID _type_id) {
    // This function is used for different conversion rules, so it can be called only after integral promotion
    switch (_type_id) {
        case INT:
        case UINT:
            return UINT;
        case LINT:
        case ULINT:
            return ULINT;
        case LLINT:
        case ULLINT:
            return ULLINT;
        default:
            ERROR("Some types are not covered (IntegerType)");
            return MAX_INT_ID;
    }
}

void BitField::init_type (IntegerTypeID it_id, uint32_t _bit_size) {
    std::shared_ptr<IntegerType> base_type = IntegerType::init(it_id);
    name = base_type->get_simple_name();
    suffix = base_type->get_int_literal_suffix();
    is_signed = base_type->get_is_signed();
    bit_size = _bit_size;
    bit_field_width = _bit_size;
    min = base_type->get_min();
    max = base_type->get_max();
    if (bit_size >= base_type->get_bit_size()) {
        bit_size = base_type->get_bit_size();
        return;
    }

    uint64_t act_max = 0;
    if (is_signed)
        act_max = pow(2, bit_size - 1) - 1;
    else
        act_max = pow(2, bit_size) - 1;
    int64_t act_min = -((int64_t) act_max) - 1;

    switch (it_id) {
        case BuiltinType::IntegerTypeID::BOOL:
            min.val.bool_val = false;
            max.val.bool_val = true;
            break;
        case BuiltinType::IntegerTypeID::CHAR:
            min.val.char_val = act_min;
            max.val.char_val = (int64_t) act_max;
            break;
        case BuiltinType::IntegerTypeID::UCHAR:
            min.val.uchar_val = 0;
            max.val.uchar_val = act_max;
            break;
        case BuiltinType::IntegerTypeID::SHRT:
            min.val.shrt_val = act_min;
            max.val.shrt_val = (int64_t) act_max;
            break;
        case BuiltinType::IntegerTypeID::USHRT:
            min.val.ushrt_val = 0;
            max.val.ushrt_val = act_max;
            break;
        case BuiltinType::IntegerTypeID::INT:
            min.val.int_val = act_min;
            max.val.int_val = (int64_t) act_max;
            break;
        case BuiltinType::IntegerTypeID::UINT:
            min.val.uint_val = 0;
            max.val.uint_val = act_max;
            break;
        case BuiltinType::IntegerTypeID::LINT:
            if (options->mode_64bit) {
                min.val.lint64_val = act_min;
                max.val.lint64_val = (int64_t) act_max;
            }
            else {
                min.val.lint32_val = act_min;
                max.val.lint32_val = (int64_t) act_max;
            }
            break;
        case BuiltinType::IntegerTypeID::ULINT:
            if (options->mode_64bit) {
                min.val.ulint64_val = 0;
                max.val.ulint64_val = act_max;
            }
            else {
                min.val.ulint32_val = 0;
                max.val.ulint32_val = act_max;
            }
            break;
         case BuiltinType::IntegerTypeID::LLINT:
            min.val.llint_val = act_min;
            max.val.llint_val = (int64_t) act_max;
            break;
         case BuiltinType::IntegerTypeID::ULLINT:
            min.val.ullint_val = 0;
            max.val.ullint_val = act_max;
            break;
        case MAX_INT_ID:
            ERROR("unsupported int type (BitField)");
            break;
    }
}

// FIXME: Check for tiger syntax
template <class T>
static std::string dbg_dump_helper (std::string name, int id, T min, T max, uint32_t bit_size, bool is_signed) {
    std::string ret = "";
    ret += "name: " + name + "\n";
    ret += "int_type_id: " + std::to_string(id) + "\n";
    ret += "min: " + std::to_string((T)min) + "\n";
    ret += "max: " + std::to_string((T)max) + "\n";
    ret += "bit_size: " + std::to_string(bit_size) + "\n";
    ret += "is_signed: " + std::to_string(is_signed) + "\n";
    return ret;
}

void TypeBOOL::dbg_dump () {
    std::cout << dbg_dump_helper<bool>(get_name(), get_int_type_id(), min.val.bool_val, max.val.bool_val, bit_size, is_signed) << std::endl;
}

void TypeCHAR::dbg_dump () {
    std::cout << dbg_dump_helper<char>(get_name(), get_int_type_id(), min.val.char_val, max.val.char_val, bit_size, is_signed) << std::endl;
}

void TypeUCHAR::dbg_dump () {
    std::cout << dbg_dump_helper<unsigned char>(get_name(), get_int_type_id(), min.val.uchar_val, max.val.uchar_val, bit_size, is_signed) << std::endl;
}

void TypeSHRT::dbg_dump () {
    std::cout << dbg_dump_helper<short>(get_name(), get_int_type_id(), min.val.shrt_val, max.val.shrt_val, bit_size, is_signed) << std::endl;
}

void TypeUSHRT::dbg_dump () {
    std::cout << dbg_dump_helper<unsigned short>(get_name(), get_int_type_id(), min.val.ushrt_val, max.val.ushrt_val, bit_size, is_signed) << std::endl;
}

void TypeINT::dbg_dump () {
    std::cout << dbg_dump_helper<int>(get_name(), get_int_type_id(), min.val.int_val, max.val.int_val, bit_size, is_signed) << std::endl;
}

void TypeUINT::dbg_dump () {
    std::cout << dbg_dump_helper<unsigned int>(get_name(), get_int_type_id(), min.val.uint_val, max.val.uint_val, bit_size, is_signed) << std::endl;
}

void TypeLINT::dbg_dump () {
    if (options->mode_64bit)
        std::cout << dbg_dump_helper<long long int>(get_name(), get_int_type_id(), min.val.lint64_val, max.val.lint64_val, bit_size, is_signed) << std::endl;
    else
        std::cout << dbg_dump_helper<int>(get_name(), get_int_type_id(), min.val.lint32_val, max.val.lint32_val, bit_size, is_signed) << std::endl;
}

void TypeULINT::dbg_dump () {
    if (options->mode_64bit)
        std::cout << dbg_dump_helper<unsigned long long int>(get_name(), get_int_type_id(), min.val.ulint64_val, max.val.ulint64_val, bit_size, is_signed) << std::endl;
    else
        std::cout << dbg_dump_helper<unsigned int>(get_name(), get_int_type_id(), min.val.ulint32_val, max.val.ulint32_val, bit_size, is_signed) << std::endl;
}

void TypeLLINT::dbg_dump () {
    std::cout << dbg_dump_helper<long long int>(get_name(), get_int_type_id(), min.val.llint_val, max.val.llint_val, bit_size, is_signed) << std::endl;
}

void TypeULLINT::dbg_dump () {
    std::cout << dbg_dump_helper<unsigned long long int>(get_name(), get_int_type_id(), min.val.ullint_val, max.val.ullint_val, bit_size, is_signed) << std::endl;
}

std::shared_ptr<BitField> BitField::generate (std::shared_ptr<Context> ctx, bool is_unnamed) {
    Type::CV_Qual cv_qual = rand_val_gen->get_rand_elem(ctx->get_gen_policy()->get_allowed_cv_qual());

    IntegerType::IntegerTypeID int_type_id = MAX_INT_ID;
    if (options->is_cxx())
        int_type_id = rand_val_gen->get_rand_id(ctx->get_gen_policy()->get_allowed_int_types());
    // In C without "J.5.8 Extended bit-field types" bit-filed can have only signed/unsigned int type
    if (options->is_c()) {
        auto search_allowed_bit_filed_type = [] (Probability<IntegerType::IntegerTypeID> prob) {
            return prob.get_id() == IntegerType::IntegerTypeID::INT ||
                   prob.get_id() == IntegerType::IntegerTypeID::UINT;
        };
        auto search_res = std::find_if(ctx->get_gen_policy()->get_allowed_int_types().begin(),
                                       ctx->get_gen_policy()->get_allowed_int_types().end(),
                                       search_allowed_bit_filed_type);
        assert(search_res != ctx->get_gen_policy()->get_allowed_int_types().end() &&
               "In C without \"J.5.8 Extended bit-field types\" bit-filed can have only signed/unsigned int type");
        int_type_id = search_res->get_id();
    }
    std::shared_ptr<IntegerType> tmp_int_type = IntegerType::init(int_type_id);

    uint32_t min_bit_size = is_unnamed ? 0 : (std::min(tmp_int_type->get_bit_size(), ctx->get_gen_policy()->get_min_bit_field_size()));

    //TODO: it cause different result for LLVM and GCC, so we limit max_bit_size to int bitsize. See pr70733
//    uint32_t max_bit_size = tmp_int_type->get_bit_size() * ctx->get_gen_policy()->get_max_bit_field_size();
    std::shared_ptr<IntegerType> int_type = IntegerType::init(Type::IntegerTypeID::INT);
    uint32_t max_bit_size = int_type->get_bit_size();

    // C doesn't allows bit-fields bigger than its base type + pr70733
    if(options->is_c())
        max_bit_size = std::min(tmp_int_type->get_bit_size(), int_type->get_bit_size());

    uint32_t bit_size = rand_val_gen->get_rand_value(min_bit_size, max_bit_size);
    return std::make_shared<BitField>(int_type_id, bit_size, cv_qual);
}

bool BitField::can_fit_in_int (BuiltinType::ScalarTypedVal val, bool is_unsigned) {
    std::shared_ptr<IntegerType> tmp_type = IntegerType::init(is_unsigned ? Type::IntegerTypeID::UINT : Type::IntegerTypeID::INT);
    bool val_is_unsig = false;
    int64_t s_val = 0;
    uint64_t u_val = 0;

    switch (val.get_int_type_id()) {
        case IntegerType::IntegerTypeID::BOOL:
        case IntegerType::IntegerTypeID::CHAR:
        case IntegerType::IntegerTypeID::UCHAR:
        case IntegerType::IntegerTypeID::SHRT:
        case IntegerType::IntegerTypeID::USHRT:
        case IntegerType::IntegerTypeID::INT:
            return true;
            break;
        case IntegerType::IntegerTypeID::UINT:
            val_is_unsig = true;
            u_val = val.val.uint_val;
            break;
        case IntegerType::IntegerTypeID::LINT:
            val_is_unsig = false;
            if (options->mode_64bit)
                s_val = val.val.lint64_val;
            else
                s_val = val.val.lint32_val;
            break;
        case IntegerType::IntegerTypeID::ULINT:
            val_is_unsig = true;
            if (options->mode_64bit)
                u_val = val.val.ulint64_val;
            else
                u_val = val.val.ulint32_val;
            break;
        case IntegerType::IntegerTypeID::LLINT:
            val_is_unsig = false;
            s_val = val.val.llint_val;
            break;
        case IntegerType::IntegerTypeID::ULLINT:
            val_is_unsig = true;
            u_val = val.val.ullint_val;
            break;
        case IntegerType::IntegerTypeID::MAX_INT_ID:
            ERROR("unsupported int type (BitField)");
    }

    int64_t s_min = 0;
    uint64_t u_min = 0;
    int64_t s_max = 0;
    uint64_t u_max = 0;

    if (is_unsigned) {
        u_min = tmp_type->get_min().val.uint_val;
        u_max = tmp_type->get_max().val.uint_val;
    }
    else {
        s_min = tmp_type->get_min().val.int_val;
        s_max = tmp_type->get_max().val.int_val;
    }

    if (val_is_unsig) {
        if (is_unsigned)
            return (u_min <= u_val) && (u_val <= u_max);
        else
            return (u_val <= (uint64_t)s_max);
    }
    else {
        if (is_unsigned)
            return ((int64_t)u_min <= s_val) && ((uint64_t)s_val <= u_max);
        else
            return (s_min <= s_val) && (s_val <= s_max);
    }
}

void BitField::dbg_dump () {
    std::string ret = "";
    ret += "name: " + name + "\n";
    ret += "int_type_id: " + std::to_string(get_int_type_id()) + "\n";
    switch (get_int_type_id()) {
        case BuiltinType::IntegerTypeID::BOOL:
            ret += "min: " + std::to_string(min.val.bool_val) + "\n";
            ret += "max: " + std::to_string(max.val.bool_val) + "\n";
            break;
        case BuiltinType::IntegerTypeID::CHAR:
            ret += "min: " + std::to_string(min.val.char_val) + "\n";
            ret += "max: " + std::to_string(max.val.char_val) + "\n";
            break;
        case BuiltinType::IntegerTypeID::UCHAR:
            ret += "min: " + std::to_string(min.val.uchar_val) + "\n";
            ret += "max: " + std::to_string(max.val.uchar_val) + "\n";
            break;
        case BuiltinType::IntegerTypeID::SHRT:
            ret += "min: " + std::to_string(min.val.shrt_val) + "\n";
            ret += "max: " + std::to_string(max.val.shrt_val) + "\n";
            break;
        case BuiltinType::IntegerTypeID::USHRT:
            ret += "min: " + std::to_string(min.val.ushrt_val) + "\n";
            ret += "max: " + std::to_string(max.val.ushrt_val) + "\n";
            break;
        case BuiltinType::IntegerTypeID::INT:
            ret += "min: " + std::to_string(min.val.int_val) + "\n";
            ret += "max: " + std::to_string(max.val.int_val) + "\n";
            break;
        case BuiltinType::IntegerTypeID::UINT:
            ret += "min: " + std::to_string(min.val.uint_val) + "\n";
            ret += "max: " + std::to_string(max.val.uint_val) + "\n";
            break;
        case BuiltinType::IntegerTypeID::LINT:
            if (options->mode_64bit) {
                ret += "min: " + std::to_string(min.val.lint64_val) + "\n";
                ret += "max: " + std::to_string(max.val.lint64_val) + "\n";
            }
            else {
                ret += "min: " + std::to_string(min.val.lint32_val) + "\n";
                ret += "max: " + std::to_string(max.val.lint32_val) + "\n";
            }
            break;
        case BuiltinType::IntegerTypeID::ULINT:
            if (options->mode_64bit) {
                ret += "min: " + std::to_string(min.val.ulint64_val) + "\n";
                ret += "max: " + std::to_string(max.val.ulint64_val) + "\n";
            }
            else {
                ret += "min: " + std::to_string(min.val.ulint32_val) + "\n";
                ret += "max: " + std::to_string(max.val.ulint32_val) + "\n";
            }
            break;
         case BuiltinType::IntegerTypeID::LLINT:
            ret += "min: " + std::to_string(min.val.llint_val) + "\n";
            ret += "max: " + std::to_string(max.val.llint_val) + "\n";
            break;
         case BuiltinType::IntegerTypeID::ULLINT:
            ret += "min: " + std::to_string(min.val.ullint_val) + "\n";
            ret += "max: " + std::to_string(max.val.ullint_val) + "\n";
            break;
        case MAX_INT_ID:
            ERROR("unsupported int type (BitField)");
            break;
    }
    ret += "bit_size: " + std::to_string(bit_size) + "\n";
    ret += "is_signed: " + std::to_string(is_signed) + "\n";
    std::cout << ret;
}

// FIXME: Check for tiger syntax
ArrayType::ArrayType(std::shared_ptr<Type> _base_type, uint32_t _size, Kind _kind) :
                     Type (Type::ARRAY_TYPE), base_type(_base_type), size(_size), kind(_kind) {
    switch (kind) {
        case C_ARR:
            break;
        case VAL_ARR:
            name = "";
            options->include_valarray = true;
            break;
        case STD_ARR:
            name = "";
            options->include_array = true;
            break;
        case STD_VEC:
            name = "";
            options->include_vector = true;
            break;
        case MAX_KIND:
            ERROR("bad array kind");
            break;
    }

    name += base_type->get_simple_name();

    // FIXME: Check for tiger syntax
    switch (kind) {
        case C_ARR:
            break;
        case VAL_ARR:
        case STD_VEC:
            name += "";
            break;
        case STD_ARR:
            name += ", " + std::to_string(size) + ">";
            break;
        case MAX_KIND:
            ERROR("bad array kind");
            break;
    }
}

std::string ArrayType::get_type_suffix() {
    return kind == C_ARR ? " [" + std::to_string(size) + "]" : "";
}

// FIXME: Check for tiger syntax
void ArrayType::dbg_dump() {
    std::string ret = "";
    ret += "kind: " + std::to_string(kind) + "\n";
    ret += "size: " + std::to_string(size) + "\n";
    ret += "full name: " + get_name();
    if (kind == C_ARR)
        ret += get_type_suffix();
    ret += "\n";
    ret += "suffix: " + get_type_suffix() + "\n";
    ret += "base type: ";
    std::cout << ret;
    base_type->dbg_dump();
    std::cout << std::endl;
}

std::shared_ptr<ArrayType> ArrayType::generate(std::shared_ptr<Context> ctx) {
    auto p = ctx->get_gen_policy();

    Type::TypeID base_type_id = rand_val_gen->get_rand_id(p->get_array_base_type_prob());
    std::shared_ptr<Type> base_type;
    if (base_type_id == TypeID::BUILTIN_TYPE || ctx->get_extern_inp_sym_table()->get_struct_types().size() == 0) {
        base_type = IntegerType::generate(ctx);
    }
    else if (base_type_id == TypeID::STRUCT_TYPE) {
        size_t struct_type_idx = rand_val_gen->get_rand_value<size_t>(
                                   0, ctx->get_extern_inp_sym_table()->get_struct_types().size() - 1);
        base_type = ctx->get_extern_inp_sym_table()->get_struct_types().at(struct_type_idx);
    }
    else
        ERROR("bad TypeID");

    uint32_t size = rand_val_gen->get_rand_value(p->get_min_array_size(), p->get_max_array_size());

    Kind kind = rand_val_gen->get_rand_id(p->get_array_kind_prob());

    return std::make_shared<ArrayType>(base_type, size, kind);
}

void PointerType::init() {
    name = pointee_type->get_simple_name() + " *";
    cv_qual = pointee_type->get_cv_qual();
    if (!pointee_type->is_ptr_type())
        return;
    std::shared_ptr<PointerType> base_ptr_type = std::static_pointer_cast<PointerType>(pointee_type);
    depth += base_ptr_type->get_depth();
}

// FIXME: Check for tiger syntax
void PointerType::dbg_dump() {
    std::cout << "Pointer type: " << name << std::endl;
    std::cout << "Pointee type:\n";
    pointee_type->dbg_dump();
}

bool yarpgen::is_pointers_compatible (std::shared_ptr<PointerType> type_a, std::shared_ptr<PointerType> type_b) {
    //TODO: will it work?
    return type_a->get_simple_name() + type_a->get_type_suffix() ==
           type_b->get_simple_name() + type_b->get_type_suffix();
}
