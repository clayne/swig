/* -----------------------------------------------------------------------------
 * See the LICENSE file for information on copyright, usage and redistribution
 * of SWIG, and the README file for authors - http://www.swig.org/release.html.
 *
 * c.cxx
 *
 * C language module for SWIG.
 * ----------------------------------------------------------------------------- */

#include <ctype.h>
#include "swigmod.h"

int SwigType_isbuiltin(SwigType *t) {
  const char* builtins[] = { "void", "short", "int", "long", "char", "float", "double", "bool", 0 };
  int i = 0;
  char *c = Char(t);
  if (!t)
    return 0;
  while (builtins[i]) {
    if (strcmp(c, builtins[i]) == 0)
      return 1;
    i++;
  }
  return 0;
}


// Private helpers, could be made public and reused from other language modules in the future.
namespace
{

enum exceptions_support {
  exceptions_support_enabled,  // Default value in C++ mode.
  exceptions_support_disabled, // Not needed at all.
  exceptions_support_imported  // Needed, but already defined in an imported module.
};

// When using scoped_dohptr, it's very simple to accidentally pass it to a vararg function, such as Printv() or Printf(), resulting in catastrophic results
// during run-time (crash or, worse, junk in the generated output), so make sure gcc warning about this, which is not enabled by default for some reason (see
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=64867 for more information), is enabled.
#ifdef __GNUC__
  #pragma GCC diagnostic error "-Wconditionally-supported"
#endif // __GNUC__

// Delete a DOH object on scope exit.
class scoped_dohptr
{
public:
  scoped_dohptr() : obj_(NULL) {}
  explicit scoped_dohptr(DOH* obj) : obj_(obj) {}
  ~scoped_dohptr() { Delete(obj_); }

  // This is an std::auto_ptr<>-like "destructive" copy ctor which allows to return objects of this type from functions.
  scoped_dohptr(scoped_dohptr const& other) : obj_(other.release()) {}

  // Same for the assignment operator.
  scoped_dohptr& operator=(scoped_dohptr const& other) {
    reset(other.release());

    return *this;
  }

  // Assignment operator takes ownership of the pointer, just as the ctor does.
  scoped_dohptr& operator=(DOH* obj) {
    reset(obj);

    return *this;
  }

  DOH* get() const { return obj_; }

  DOH* release() const /* not really */ {
    DOH* obj = obj_;
    const_cast<DOH*&>(const_cast<scoped_dohptr*>(this)->obj_) = NULL;
    return obj;
  }

  void reset(DOH* obj = NULL) {
    if (obj != obj_) {
      Delete(obj_);
      obj_ = obj;
    }
  }

  operator DOH*() const { return obj_; }

protected:
  DOH* obj_;
};

// Wrapper for a DOH object which can be owned or not.
class maybe_owned_dohptr : public scoped_dohptr
{
public:
  explicit maybe_owned_dohptr(DOH* obj = NULL) : scoped_dohptr(obj), owned_(true) {}

  maybe_owned_dohptr(maybe_owned_dohptr const& other) : scoped_dohptr(other) {
    owned_ = other.owned_;

    // We can live other.owned_ unchanged, as its pointer is null now anyhow.
  }

  maybe_owned_dohptr& operator=(maybe_owned_dohptr const& other) {
    reset(other.release());
    owned_ = other.owned_;

    return *this;
  }

  ~maybe_owned_dohptr() {
    if (!owned_)
      obj_ = NULL; // Prevent it from being deleted by the base class dtor.
  }

  void assign_owned(DOH* obj) {
    reset(obj);
  }

  void assign_non_owned(DOH* obj) {
    reset(obj);
    owned_ = false;
  }

private:
  bool owned_;
};


// Helper class setting the given pointer to the given value in its ctor and resetting it in the dtor.
//
// Used to non-intrusively set a pointer to some object only during this object life-time.
template <typename T>
class temp_ptr_setter
{
public:
  // Pointer must be non-null, its current value is restored when this object is destroyed.
  temp_ptr_setter(T* ptr, T value) : ptr_(ptr), value_orig_(*ptr) {
    *ptr_ = value;
  }

  ~temp_ptr_setter() {
    *ptr_ = value_orig_;
  }

private:
  T* const ptr_;
  T const value_orig_;

  // Non copyable.
  temp_ptr_setter(const temp_ptr_setter&);
  temp_ptr_setter& operator=(const temp_ptr_setter&);
};


// Helper class to output "begin" fragment in the ctor and "end" in the dtor.
class begin_end_output_guard
{
public:
  begin_end_output_guard(File* f, const_String_or_char_ptr begin, const_String_or_char_ptr end)
    : f_(f),
      end_(NewString(end))
  {
    String* const s = NewString(begin);
    Dump(s, f_);
    Delete(s);
  }

  ~begin_end_output_guard()
  {
    Dump(end_, f_);
    Delete(end_);
  }

private:
  // Non copyable.
  begin_end_output_guard(const begin_end_output_guard&);
  begin_end_output_guard& operator=(const begin_end_output_guard&);

  File* const f_;
  String* const end_;
};

// Subclass to output extern "C" guards when compiling as C++.
class cplusplus_output_guard : private begin_end_output_guard
{
public:
  explicit cplusplus_output_guard(File* f)
    : begin_end_output_guard(
        f,
        "#ifdef __cplusplus\n"
        "extern \"C\" {\n"
        "#endif\n\n",
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n\n"
      )
  {
  }
};

// String containing one indentation level for the generated code.
const char* const cindent = "  ";

// Returns the non-owned string to the name of the class or enum to use in C wrappers.
String* get_c_proxy_name(Node* n) {
  String *proxyname = Getattr(n, "proxyname");
  if (!proxyname) {
    String *symname = Getattr(n, "sym:name");
    String *nspace = Getattr(n, "sym:nspace");

    if (nspace) {
      scoped_dohptr nspace_mangled(Swig_string_mangle(nspace));
      proxyname = NewStringf("%s_%s", (DOH*)nspace_mangled, symname);
    } else {
      proxyname = Swig_name_type(symname);
    }
    Setattr(n, "proxyname", proxyname);

    Delete(proxyname); // It stays alive because it's referenced by the hash.
  }

  return proxyname;
}

// Returns the first named "import" node under the given one (which must be non-NULL). May return NULL.
Node* find_first_named_import(Node* parent) {
  for (Node* n = firstChild(parent); n; n = nextSibling(n)) {
    if (Cmp(nodeType(n), "import") == 0) {
      // We've almost succeeded, but there are sometimes some weird unnamed import modules that don't really count for our purposes, so skip them.
      if (Getattr(n, "module"))
	return n;
    } else if (Cmp(nodeType(n), "include") == 0) {
      // Recurse into this node as included files may contain imports too.
      if (Node* const import = find_first_named_import(n))
	return import;
    } else {
      // We consider that import nodes can only occur in the global scope, some don't bother recursing here. If this turns out to be false, we'd just need to
      // start doing it.
    }
  }

  return NULL;
}


/*
  Struct containing information needed only for generating C++ wrappers.
*/
struct cxx_wrappers
{
  // Default ctor doesn't do anything, use initialize() if C++ wrappers really need to be generated.
  cxx_wrappers() :
    except_check_start(NULL), except_check_end(NULL),
    sect_types(NULL), sect_decls(NULL), sect_impls(NULL) {
  }

  void initialize() {
    sect_types = NewStringEmpty();
    sect_decls = NewStringEmpty();
    sect_impls = NewStringEmpty();
  }

  // This function must be called after initialize(). The two can't be combined because we don't yet know if we're going to use exceptions or not when we
  // initialize the object of this class in C::main(), so this one is called later from C::top().
  void initialize_exceptions(exceptions_support support) {
    switch (support) {
      case exceptions_support_enabled:
	// Generate the functions which will be used in all wrappers to check for the exceptions only in this case, i.e. do not do it if they're already defined
	// in another module imported by this one.
	Printv(sect_impls,
	  "inline void swig_check() {\n",
	  cindent, "if (SWIG_CException* swig_ex = SWIG_CException::get_pending()) {\n",
	  cindent, cindent, "SWIG_CException swig_ex_copy{*swig_ex};\n",
	  cindent, cindent, "SWIG_CException::reset_pending();\n",
	  cindent, cindent, "throw swig_ex_copy;\n",
	  cindent, "}\n",
	  "}\n\n",
	  "template <typename T> T swig_check(T x) {\n",
	  cindent, "swig_check();\n",
	  cindent, "return x;\n",
	  "}\n\n",
	  NIL
	);

	// fall through

      case exceptions_support_imported:
	except_check_start = "swig_check(";
	except_check_end = ")";
	break;

      case exceptions_support_disabled:
	except_check_start =
	except_check_end = "";
	break;
    }
  }

  bool is_initialized() const { return sect_types != NULL; }


  // Used for generating exception checks around the calls, see initialize_exceptions().
  const char* except_check_start;
  const char* except_check_end;


  // The order of the members here is the same as the order in which they appear in the output file.

  // This section contains forward declarations of the classes.
  String* sect_types;

  // Full declarations of the classes.
  String* sect_decls;

  // Implementation of the classes.
  String* sect_impls;
};

/*
  cxx_class_wrapper

  Outputs the declaration of the class wrapping the given one if we're generating C++ wrappers, i.e. if the provided cxx_wrappers object is initialized.
*/
class cxx_class_wrapper
{
public:
  // If the provided cxx_wrappers object is not initialized, this class doesn't do anything.
  //
  // The node pointer must be valid, point to a class and remain valid for the lifetime of this object.
  cxx_class_wrapper(const cxx_wrappers& cxx_wrappers, Node* n) : cxx_wrappers_(cxx_wrappers) {
    class_node_ = NULL;

    rtype_desc_ =
    ptype_desc_ = NULL;

    if (!cxx_wrappers_.is_initialized())
      return;

    scoped_dohptr base_classes(NewStringEmpty());
    if (List *baselist = Getattr(n, "bases")) {
      for (Iterator i = First(baselist); i.item; i = Next(i)) {
	if (Checkattr(i.item, "feature:ignore", "1"))
	  continue;

	if (first_base_) {
	  Swig_warning(WARN_C_UNSUPPORTTED, Getfile(n), Getline(n),
	    "Multiple inheritance not supported yet, skipping C++ wrapper generation for %s\n",
	    Getattr(n, "sym:name")
	  );

	  // Return before initializing class_node_, so that the dtor won't output anything neither.
	  return;
	}

	first_base_ = Copy(i.item);
      }

      Printv(base_classes, " : public ", Getattr(first_base_, "sym:name"), NIL);
    }

    Printv(cxx_wrappers_.sect_types,
      "class ", Getattr(n, "sym:name"), ";\n",
      NIL
    );

    Printv(cxx_wrappers_.sect_decls,
      "class ", Getattr(n, "sym:name"), base_classes.get(), " {\n"
      "public:\n",
      NIL
    );

    class_node_ = n;
    has_copy_ctor_ = false;
  }

  // Get indentation used inside this class declaration.
  const char* get_indent() const {
    // Currently we always use a single level of indent, but this would need to change if/when nested classes are supported.
    //
    // As the first step, we should probably change all occurrences of "cindent" in this class itself to use get_indent() instead.
    return cindent;
  }

  // Emit wrapper of a member function.
  void emit_member_function(Node* n) {
    if (!class_node_)
      return;

    // We don't need to redeclare functions inherited from the base class, as we use real inheritance.
    if (Getattr(n, "c:inherited_from"))
      return;

    // Also ignore friend function declarations: they appear inside the class, but we shouldn't generate any wrappers for them.
    if (Checkattr(n, "storage", "friend"))
      return;

    // As mentioned elsewhere, we can't use Swig_storage_isstatic() here because the "storage" attribute is temporarily saved in another view when this
    // function is being executed, so rely on another attribute to determine if it's a static function instead.
    const bool is_member = Checkattr(n, "ismember", "1");
    const bool is_static = is_member && Getattr(n, "cplus:staticbase");
    const bool is_ctor = Checkattr(n, "nodeType", "constructor");

    // Deal with the return type: it may be different from the type of the C wrapper function if it involves objects, and so we may need to add a cast.

    type_desc rtype_desc;
    if (SwigType_type(Getattr(n, "type")) != T_VOID) {
      rtype_desc = lookup_cxx_ret_type(n);
      if (!rtype_desc.type()) {
	Swig_warning(WARN_C_TYPEMAP_CTYPE_UNDEF, Getfile(n), Getline(n),
	  "No ctype typemap defined for the return type \"%s\" of %s\n",
	  SwigType_str(Getattr(n, "type"), NULL),
	  Getattr(n, "sym:name")
	);
	return;
      }
    } else {
      // There is no need to do anything else with "void" and we don't even need "return" for it.
      rtype_desc.set_void_type();
    }

    // We also need the list of parameters to take in the C++ function being generated and the list of them to pass to the C wrapper.
    scoped_dohptr parms_cxx(NewStringEmpty());
    scoped_dohptr parms_call(NewStringEmpty());

    Parm* p = Getattr(n, "parms");
    if (p && is_member && !is_ctor && !is_static) {
      // We should have "this" as the first parameter and we need to just skip it, as we handle it specially in C++ wrappers.
      if (Checkattr(p, "name", "self")) {
	p = nextSibling(p);
      } else {
	// This is not supposed to happen, so warn if it does.
	Swig_warning(WARN_C_UNSUPPORTTED, Getfile(n), Getline(n),
	  "Unexpected first parameter \"%s\" in %s\n",
	  Getattr(p, "name"),
	  Getattr(n, "sym:name"));
      }
    }

    for (; p; p = nextSibling(p)) {
      // Static variables use fully qualified names, so we can't just use the name directly.
      scoped_dohptr name_ptr;
      String* name = Getattr(p, "name");
      if (!name) {
	// Parameters can also not have any names at all, in which case we use auto-generated one.
	name = Getattr(p, "lname");
      } else if (Strstr(name, "::")) {
	name_ptr = Swig_scopename_last(name);
	name = name_ptr.get();
      }

      const type_desc ptype_desc = lookup_cxx_parm_type(p);
      if (!ptype_desc.type()) {
	Swig_warning(WARN_C_TYPEMAP_CTYPE_UNDEF, Getfile(p), Getline(p),
	  "No ctype typemap defined for the parameter \"%s\" of %s\n",
	  name,
	  Getattr(n, "sym:name")
	);
	return;
      }

      if (Len(parms_cxx))
	Append(parms_cxx, ", ");
      Printv(parms_cxx, ptype_desc.type(), " ", name, NIL);

      if (Len(parms_call))
	Append(parms_call, ", ");
      Printv(parms_call, ptype_desc.wrap_start(), name, ptype_desc.wrap_end(), NIL);
    }

    // Avoid checking for exceptions unnecessarily. Note that this is more than an optimization: we'd get into infinite recursion if we checked for exceptions
    // thrown by members of SWIG_CException itself if we didn't do it.
    const char* except_check_start = cxx_wrappers_.except_check_start;
    const char* except_check_end = cxx_wrappers_.except_check_end;
    if (*except_check_start) {
      if (Checkattr(n, "noexcept", "true") || (Checkattr(n, "throw", "1") && !Getattr(n, "throws"))) {
	except_check_start =
	except_check_end = "";
      }
    }

    // For some reason overloaded functions use fully-qualified name, so we can't just use the name directly.
    scoped_dohptr name_ptr(Swig_scopename_last(Getattr(n, "name")));
    String* const name = name_ptr.get();
    String* const wname = Getattr(n, "wrap:name");

    String* const classname = Getattr(class_node_, "sym:name");

    if (Checkattr(n, "kind", "variable")) {
      if (Checkattr(n, "memberget", "1")) {
	Printv(cxx_wrappers_.sect_decls,
	  cindent, rtype_desc.type(), " ", name, "() const "
	  "{ ",
	    "return ", rtype_desc.wrap_start(),
	    Getattr(n, "sym:name"), "(swig_self())",
	    rtype_desc.wrap_end(),
	  "; }\n",
	  NIL
	);
      } else if (Checkattr(n, "memberset", "1")) {
	Printv(cxx_wrappers_.sect_decls,
	  cindent, "void ", name, "(", parms_cxx.get(), ") "
	  "{ ", Getattr(n, "sym:name"), "(swig_self(), ", parms_call.get(), "); }\n",
	  NIL
	);
      } else if (Checkattr(n, "varget", "1")) {
	Printv(cxx_wrappers_.sect_decls,
	  cindent, "static ", rtype_desc.type(), " ", name, "() "
	  "{ ",
	    "return ", rtype_desc.wrap_start(),
	    Getattr(n, "sym:name"), "()",
	    rtype_desc.wrap_end(),
	  "; }\n",
	  NIL
	);
      } else if (Checkattr(n, "varset", "1")) {
	Printv(cxx_wrappers_.sect_decls,
	  cindent, "static void ", name, "(", parms_cxx.get(), ") "
	  "{ ", Getattr(n, "sym:name"), "(", parms_call.get(), "); }\n",
	  NIL
	);
      } else {
	Swig_warning(WARN_C_UNSUPPORTTED, Getfile(n), Getline(n),
	  "Not generating C++ wrappers for variable %s\n",
	  Getattr(n, "sym:name")
	);
      }
    } else if (is_ctor) {
      // Delegate to the ctor from opaque C pointer taking ownership of the object.
      Printv(cxx_wrappers_.sect_decls,
	cindent, classname, "(", parms_cxx.get(), ");\n",
	NIL
      );

      Printv(cxx_wrappers_.sect_impls,
	"inline ", classname, "::", classname, "(", parms_cxx.get(), ") : ",
	classname, "{",
	  except_check_start,
	    wname, "(", parms_call.get(), ")",
	  except_check_end,
	"} {}\n",
	NIL
      );

      // Remember that we had a copy ctor.
      if (Checkattr(n, "copy_constructor", "1"))
	has_copy_ctor_ = true;
    } else if (Checkattr(n, "nodeType", "destructor")) {
      if (first_base_) {
	// Delete the pointer and reset the ownership flag to ensure that the base class doesn't do it again.
	Printv(cxx_wrappers_.sect_decls,
	  cindent, get_virtual_prefix(n), "~", classname, "() {\n",
	  cindent, cindent, "if (swig_owns_self_) {\n",
	  cindent, cindent, cindent, wname, "(swig_self());\n",
	  cindent, cindent, cindent, "swig_owns_self_ = false;\n",
	  cindent, cindent, "}\n",
	  cindent, "}\n",
	  NIL
	);
      } else {
	// Slightly simplified version for classes without base classes, as we don't need to reset swig_self_ then.
	Printv(cxx_wrappers_.sect_decls,
	  cindent, get_virtual_prefix(n), "~", classname, "() {\n",
	  cindent, cindent, "if (swig_owns_self_)\n",
	  cindent, cindent, cindent, wname, "(swig_self_);\n",
	  cindent, "}\n",
	  NIL
	);
      }
    } else if (is_member) {
      // Wrapper parameters list may or not include "this" pointer and may or not have other parameters, so construct it piecewise for simplicity.
      scoped_dohptr wparms(NewStringEmpty());
      if (!is_static)
	Append(wparms, "swig_self()");
      if (Len(parms_call)) {
	if (Len(wparms))
	  Append(wparms, ", ");
	Append(wparms, parms_call);
      }

      Printv(cxx_wrappers_.sect_decls,
	cindent,
	is_static ? "static " : get_virtual_prefix(n), rtype_desc.type(), " ",
	name, "(", parms_cxx.get(), ")",
	get_const_suffix(n), ";\n",
	NIL
      );

      Printv(cxx_wrappers_.sect_impls,
	"inline ", rtype_desc.type(), " ",
	classname, "::", name, "(", parms_cxx.get(), ")",
	get_const_suffix(n),
	" { ",
	NIL
      );

      if (rtype_desc.is_void()) {
	Printv(cxx_wrappers_.sect_impls,
	  wname, "(", wparms.get(), ")",
	  NIL
	);

	if (*except_check_start) {
	  Printv(cxx_wrappers_.sect_impls,
	    "; ",
	    except_check_start,
	    except_check_end,
	    NIL
	  );
	}
      } else {
	Printv(cxx_wrappers_.sect_impls,
	  "return ",
	  rtype_desc.wrap_start(),
	    except_check_start,
	      wname, "(", wparms.get(), ")",
	    except_check_end,
	  rtype_desc.wrap_end(),
	  NIL
	);
      }

      Printv(cxx_wrappers_.sect_impls,
	"; }\n",
	NIL
      );
    } else {
      // This is something we don't know about
      Swig_warning(WARN_C_UNSUPPORTTED, Getfile(n), Getline(n),
	"Not generating C++ wrappers for %s\n",
	Getattr(n, "sym:name")
      );
    }
  }

  ~cxx_class_wrapper() {
    // Don't do anything if generation of the wrapper for this class was disabled in ctor.
    if (!class_node_)
      return;

    // This is the name used for the class pointers in C wrappers.
    scoped_dohptr c_class_ptr = get_c_class_ptr(class_node_);

    String* const classname = Getattr(class_node_, "sym:name");

    // We need to generate a ctor from the C object pointer, which is required to be able to create objects of this class from pointers created by C wrappers
    // and also by any derived classes.
    Printv(cxx_wrappers_.sect_decls,
      "\n",
      cindent, "explicit ", classname, "(", c_class_ptr.get(), " swig_self, "
	"bool swig_owns_self = true) noexcept : ",
      NIL
    );

    if (first_base_) {
      // In this case we delegate to the base class ctor, but need a cast because it expects a different pointer type (as these types are opaque, there is no
      // relationship between them).
      Printv(cxx_wrappers_.sect_decls,
	Getattr(first_base_, "sym:name"),
	"{(", get_c_class_ptr(first_base_).get(), ")swig_self, swig_owns_self}",
	NIL
      );
    } else {
      // Just initialize our own field.
      Printv(cxx_wrappers_.sect_decls,
	"swig_self_{swig_self}, swig_owns_self_{swig_owns_self}",
	NIL
      );
    }

    Append(cxx_wrappers_.sect_decls, " {}\n");

    // If the class doesn't have a copy ctor, forbid copying it: we currently must do it even if the original class has a perfectly cromulent implicit copy ctor
    // because we don't wrap it and copying would use the trivial ctor that would just copy the swig_self_ pointer resulting in double destruction of it later.
    // To fix this, we would need to always provide our own C wrapper for the copy ctor, which is not something we do currently.
    if (!has_copy_ctor_) {
      Printv(cxx_wrappers_.sect_decls,
	cindent, classname, "(", classname, " const&) = delete;\n",
	NIL
      );
    }

    // We currently never wrap the assignment operator, so we have to always disable it for the same reason we disable the copy ctor above.
    // It would definitely be nice to provide the assignment, if possible.
    Printv(cxx_wrappers_.sect_decls,
      cindent, classname, "& operator=(", classname, " const&) = delete;\n",
      NIL
    );

    // OTOH we may always provide move ctor and assignment, as we can always implement them trivially ourselves.
    if (first_base_) {
      Printv(cxx_wrappers_.sect_decls,
	cindent, classname, "(", classname, "&& obj) = default;\n",
	cindent, classname, "& operator=(", classname, "&& obj) = default;\n",
	NIL
      );
    } else {
      Printv(cxx_wrappers_.sect_decls,
	cindent, classname, "(", classname, "&& obj) noexcept : "
	"swig_self_{obj.swig_self_}, swig_owns_self_{obj.swig_owns_self_} { "
	"obj.swig_owns_self_ = false; "
	"}\n",
	cindent, classname, "& operator=(", classname, "&& obj) noexcept { "
	"swig_self_ = obj.swig_self_; swig_owns_self_ = obj.swig_owns_self_; "
	"obj.swig_owns_self_ = false; "
	"return *this; "
	"}\n",
	NIL
      );
    }

    // We also need a swig_self() method for accessing the C object pointer.
    Printv(cxx_wrappers_.sect_decls,
      cindent, c_class_ptr.get(), " swig_self() const noexcept ",
      NIL
    );

    if (first_base_) {
      // If we have a base class, we reuse its existing "self" pointer.
      Printv(cxx_wrappers_.sect_decls,
	"{ return (", c_class_ptr.get(), ")", Getattr(first_base_, "sym:name"), "::swig_self(); }\n",
	NIL
      );
    } else {
      // We use our own pointer, which we also have to declare, together with the ownership flag.
      //
      // Perhaps we could avoid having a separate bool flag by reusing the low-order bit of the pointer itself as the indicator of ownership and masking it when
      // retrieving it here in the future. If we decide to implement this optimization, the code generated here should be the only thing that would need to
      // change.
      Printv(cxx_wrappers_.sect_decls,
	"{ return swig_self_; }\n",
	cindent, c_class_ptr.get(), " swig_self_;\n",
	cindent, "bool swig_owns_self_;\n",
	NIL
      );
    }

    Printv(cxx_wrappers_.sect_decls,
      "};\n"
      "\n",
      NIL
    );
  }


  // This function is called from C::replaceSpecialVariables() but only does something non-trivial when it's called by our own lookup_cxx_xxx_type() functions.
  bool replaceSpecialVariables(String *method, String *tm, Parm *parm) {
    if (!ptype_desc_ && !rtype_desc_)
      return false;

    if (Cmp(method, "ctype") != 0) {
      Swig_warning(WARN_C_UNSUPPORTTED, input_file, line_number, "Unsupported %s typemap %s\n", method, tm);
      return false;
    }

    if (SwigType *type = Getattr(parm, "type")) {
      if (ptype_desc_)
	ptype_desc_->set_type(type);
      if (rtype_desc_)
	rtype_desc_->set_type(type);

      do_resolve_type(parm, tm, ptype_desc_, rtype_desc_);
    }

    return true;
  }

private:
  // This struct contains the type itself and, optionally, wrappers around expressions of this type: start part goes before the expression and the end part
  // after it (and both parts may be empty).
  class type_desc
  {
  public:
    // Ctor initializes the object to an empty/unknown state, call set_type() later to finish initialization.
    type_desc() : wrap_start_(NewStringEmpty()), wrap_end_(NewStringEmpty()) {}

    // String must be non-null.
    void set_type(String* type) { type_ = Copy(type); }
    void set_void_type() { type_ = NewString("void"); }

    bool is_void() const { return type_ && Cmp(type_, "void") == 0; }

    // If this one returns NULL, it means that we don't have any type information at all.
    String* type() const { return type_; }

    // These ones are always non-NULL (but possibly empty).
    String* wrap_start() const { return wrap_start_; }
    String* wrap_end() const { return wrap_end_; }

  private:
    scoped_dohptr type_;
    scoped_dohptr wrap_start_;
    scoped_dohptr wrap_end_;
  };


  // Various helpers.

  // Return the string containing the pointer type used for representing the objects of the given class in the C wrappers.
  //
  // Returned value includes "*" at the end.
  static scoped_dohptr get_c_class_ptr(Node* class_node) {
    return scoped_dohptr(NewStringf("SwigObj_%s*", get_c_proxy_name(class_node)));
  }

  // Return "virtual " if this is a virtual function, empty string otherwise.
  static const char* get_virtual_prefix(Node* n) {
    return Checkattr(n, "storage", "virtual") ? "virtual " : "";
  }

  // Return " const" if this is a const function, empty string otherwise.
  static const char* get_const_suffix(Node* n) {
    String* const qualifier = Getattr(n, "qualifier");
    return qualifier && strncmp(Char(qualifier), "q(const)", 8) == 0 ? " const" : "";
  }

  // Replace "resolved_type" occurrences in the string with the value corresponding to the given type.
  //
  // Also fills in the start/end wrapper parts of the provided type descriptions if they're not null, with the casts needed to translate from C type to C++ type
  // (this is used for the parameters of C++ functions, hence the name) and from C types to C++ types (which is used for the function return values).
  static void do_resolve_type(Node* n, String* s, type_desc* ptype_desc, type_desc* rtype_desc) {
    enum TypeKind
    {
      Type_Ptr,
      Type_Ref,
      Type_Obj,
      Type_Max
    } typeKind = Type_Max;

    // These correspond to the typemaps for SWIGTYPE*, SWIGTYPE& and SWIGTYPE, respectively, defined in c.swg.
    static const char* typemaps[Type_Max] = {
      "$resolved_type*",
      "$*resolved_type*",
      "$&resolved_type*",
    };

    for (int i = 0; i < Type_Max; ++i) {
      if (Strstr(s, typemaps[i])) {
	typeKind = static_cast<TypeKind>(i);
	break;
      }
    }

    if (typeKind == Type_Max) {
      if (Strstr(s, "resolved_type")) {
	Swig_warning(WARN_C_UNSUPPORTTED, input_file, line_number,
	  "Unsupported typemap used for \"%s\"\n",
	  Getattr(n, "sym:name")
	);
      }

      return;
    }

    String* const type = Getattr(n, "type");
    scoped_dohptr resolved_type(SwigType_typedef_resolve_all(type));
    scoped_dohptr stripped_type(SwigType_strip_qualifiers(resolved_type));

    scoped_dohptr typestr;
    String* classname;
    if (Node* const class_node = Language::instance()->classLookup(stripped_type)) {
      typestr = SwigType_str(type, 0);
      classname = Getattr(class_node, "sym:name");

      // We don't use namespaces, but the type may contain them, so get rid of them by replacing the base type name, which is fully qualified, with just the
      // class name, which is not.
      scoped_dohptr basetype(SwigType_base(type));
      scoped_dohptr basetypestr(SwigType_str(basetype, 0));
      if (Cmp(basetypestr, classname) != 0) {
	Replaceall(typestr, basetypestr, classname);
      }
    } else {
      // This is something unknown, so just use an opaque typedef already declared in C wrappers section for it.
      typestr = NewStringf("SWIGTYPE%s*", SwigType_manglestr(stripped_type));
      classname = NULL;
    }

    switch (typeKind) {
      case Type_Ptr:
	if (ptype_desc) {
	  Append(ptype_desc->wrap_end(), "->swig_self()");
	}

	if (rtype_desc) {
	  if (classname) {
	    // We currently assume that all pointers are new, which is probably wrong.
	    //
	    // We generate here an immediately-invoked lambda, as we need something that can appear after a "return".
	    Append(rtype_desc->wrap_start(), "[=] { auto swig_res = ");
	    Printv(rtype_desc->wrap_end(),
	      "; "
	      "return swig_res ? new ", classname, "(swig_res) : nullptr; }()",
	      NIL
	    );
	  }
	}
	break;

      case Type_Ref:
	if (rtype_desc) {
	  if (classname) {
	    // We can't return a reference, as this requires an existing object and we don't have any, so we have to return an object instead, and this object
	    // must be constructed using the special ctor not taking the pointer ownership.
	    typestr = Copy(classname);

	    Printv(rtype_desc->wrap_start(),
	      classname, "{",
	      NIL
	    );
	    Printv(rtype_desc->wrap_end(),
	      ", false}",
	      NIL
	    );
	  } else {
	    // We can't do anything at all in this case.
	    Swig_error(input_file, line_number, "Unknown reference return type \"%s\"\n", typestr.get());
	  }
	}

	if (ptype_desc) {
	  Append(ptype_desc->wrap_end(), ".swig_self()");
	}
	break;

      case Type_Obj:
	if (rtype_desc) {
	  if (classname) {
	    // The pointer returned by C function wrapping a function returning an object should never be null unless an exception happened.
	    Printv(rtype_desc->wrap_start(),
	      typestr.get(), "(",
	      NIL
	    );
	    Append(rtype_desc->wrap_end(), ")");
	  } else {
	    Swig_error(input_file, line_number, "Unknown reference return type \"%s\"\n", typestr.get());
	  }
	}

	if (ptype_desc) {
	  // It doesn't seem like it can ever be useful to pass an object by value to a wrapper function and it can fail if it doesn't have a copy ctor (see
	  // code related to has_copy_ctor_ in our dtor above), so always pass it by const reference instead.
	  Append(typestr, " const&");

	  Append(ptype_desc->wrap_end(), ".swig_self()");
	}
	break;

      case Type_Max:
	// Unreachable, but keep here to avoid -Wswitch warnings.
	assert(0);
    }

    Replaceall(s, typemaps[typeKind], typestr);
  }

  type_desc lookup_cxx_parm_type(Node* n) {
    type_desc ptype_desc;

    // Ensure our own replaceSpecialVariables() is used for $typemap() expansion.
    temp_ptr_setter<type_desc*> set(&ptype_desc_, &ptype_desc);

    if (String* type = Swig_typemap_lookup("ctype", n, "", NULL)) {
      ptype_desc.set_type(type);
      do_resolve_type(n, ptype_desc.type(), &ptype_desc, NULL);
    }

    return ptype_desc;
  }

  type_desc lookup_cxx_ret_type(Node* n) {
    type_desc rtype_desc;

    // As above, ensure our replaceSpecialVariables() is used.
    temp_ptr_setter<type_desc*> set(&rtype_desc_, &rtype_desc);

    if (String* type = Swig_typemap_lookup("ctype", n, "", NULL)) {
      rtype_desc.set_type(type);
      do_resolve_type(n, rtype_desc.type(), NULL, &rtype_desc);
    }

    return rtype_desc;
  }


  const cxx_wrappers& cxx_wrappers_;

  // The class node itself, left null only if we skip generating wrappers for it for whatever reason.
  Node* class_node_;

  // We currently don't support generating C++ wrappers for classes using multiple inheritance. This could be implemented, with some tweaks to allow
  // initializing the other base classes after creating the most-derived object, but hasn't been done yet. Until then we store just the first base class (if
  // any, this member can also be null).
  scoped_dohptr first_base_;

  // These pointers are temporarily set to non-null value only while expanding a typemap for C++ wrappers, see replaceSpecialVariables().
  type_desc* ptype_desc_;
  type_desc* rtype_desc_;

  // True if the class defines an explicit copy ctor.
  bool has_copy_ctor_;


  // Non copyable.
  cxx_class_wrapper(const cxx_class_wrapper&);
  cxx_class_wrapper& operator=(const cxx_class_wrapper&);
};

} // anonymous namespace

class C:public Language {
  static const char *usage;

  // These files contain types used by the wrappers declarations and the declarations themselves and end up in the output header file.
  String *sect_wrappers_types;
  String *sect_wrappers_decl;

  // This one contains wrapper functions definitions and end up in the output C++ file.
  String *sect_wrappers;

  String *empty_string;

  // Namespace used for the C++ wrappers, set from -namespace command-line option if specified or from the module name otherwise.
  String *ns_cxx;

  // Prefix used for all symbols, if non-null. If ns_cxx was specified, it is a mangled version of it.
  String *ns_prefix;

  // Name of the module, used as a prefix for module-level symbols if ns_prefix is null.
  String *module_name;

  // Name of the output header, set in top().
  String *outfile_h;

  // Used only while generating wrappers for an enum and contains the prefix, ending with underscore, to use for enum elements or is empty.
  scoped_dohptr enum_prefix_;

  // Used only while generating wrappers for an enum, as we don't know if enum will have any elements or not in advance and we must not generate an empty enum,
  // so we accumulate the full declaration here and then write it to sect_wrappers_types at once only if there are any elements.
  String *enum_decl;

  // Selects between the wrappers (public) declarations and (private) definitions.
  enum {
    output_wrapper_decl,
    output_wrapper_def
  } current_output;

  // Selects between various kinds of needed support for exception-related code.
  exceptions_support exceptions_support_;

  // This object contains information necessary only for C++ wrappers generation, use its is_initialized() to check if this is being done.
  cxx_wrappers cxx_wrappers_;

  // Non-owning pointer to the current C++ class wrapper if we're currently generating one or NULL.
  cxx_class_wrapper* cxx_class_wrapper_;

  // This is parallel to enum_decl but for C++ enum declaration.
  String *cxx_enum_decl;

  // An extra indent level needed for nested C++ enums.
  const char* cxx_enum_indent;

public:

  /* -----------------------------------------------------------------------------
   * C()
   * ----------------------------------------------------------------------------- */

  C() :
    empty_string(NewString("")),
    ns_cxx(NULL),
    ns_prefix(NULL),
    module_name(NULL),
    outfile_h(NULL),
    cxx_class_wrapper_(NULL)
  {
  }

  ~C()
  {
    Delete(ns_cxx);
    Delete(ns_prefix);
  }

  // Construct the name to be used for a function with the given name in C wrappers.
  //
  // The returned string must be freed by caller.
  maybe_owned_dohptr getFunctionWrapperName(Node *n, String *name) const
  {
    maybe_owned_dohptr wname;

    // The basic idea here is that for class members we don't need to use any prefix at all, as they're already prefixed by the class name, which has the
    // appropriate prefix, but we need to use a prefix for the other symbols.
    //
    // However there are a couple of special cases complicating this:
    //
    //  - Friend functions are declared inside the class, but are not member functions, so we have to check for both the current class and "ismember" property.
    //  - Destructors and implicitly generated constructors don't have "ismember" for some reason, so we need to check for them specifically.
    //  - Variable getters and setters don't need to use the prefix as they don't clash with anything.
    if ((getCurrentClass() &&
	  (Checkattr(n, "ismember", "1") ||
	    Checkattr(n, "nodeType", "constructor") ||
	      Checkattr(n, "nodeType", "destructor"))) ||
-               Checkattr(n, "varget", "1") || Checkattr(n, "varset", "1")) {
      wname.assign_non_owned(name);
      return wname;
    }

    // Use namespace as the prefix if feature:nspace is in use.
    scoped_dohptr scopename_prefix;
    if (GetFlag(parentNode(n), "feature:nspace")) {
      scopename_prefix = Swig_scopename_prefix(Getattr(n, "name"));
      if (scopename_prefix) {
	scoped_dohptr mangled_prefix(Swig_string_mangle(scopename_prefix));
	scopename_prefix = mangled_prefix;
      }
    }

    // Fall back to the module name if we don't use feature:nspace and don't have the global prefix neither.
    //
    // Note that we really, really need to use some prefix, as wrapper function can't have the same name as the original function being wrapped.
    String* const prefix = scopename_prefix
      ? scopename_prefix
      : ns_prefix
	? ns_prefix
	: module_name;

    wname.assign_owned(NewStringf("%s_%s", prefix, name));
    return wname;
  }

  /* -----------------------------------------------------------------------------
   * getClassProxyName()
   *
   * Test to see if a type corresponds to something wrapped with a proxy class.
   * Return NULL if not, otherwise the proxy class name to be freed by the caller.
   * ----------------------------------------------------------------------------- */

   String *getClassProxyName(SwigType *t) {
     Node *n = classLookup(t);

    return n ? Copy(get_c_proxy_name(n)) : NULL;

   }

  /* -----------------------------------------------------------------------------
   * getEnumName()
   *
   * Return the name to use for the enum in the generated code.
   * Also caches it in the node for subsequent access.
   * Returns NULL if the node doesn't correspond to an enum.
   * ----------------------------------------------------------------------------- */

  String *getEnumName(Node *n) {
    String *enumname = NULL;
    if (n) {
      enumname = Getattr(n, "enumname");
      if (!enumname) {
        String *symname = Getattr(n, "sym:name");
        if (symname) {
          // Add in class scope when referencing enum if not a global enum
          String *proxyname = 0;
          if (String *name = Getattr(n, "name")) {
	    if (String *scopename_prefix = Swig_scopename_prefix(name)) {
	      proxyname = getClassProxyName(scopename_prefix);
	      Delete(scopename_prefix);
	    }
	  }
          if (proxyname) {
            enumname = NewStringf("%s_%s", proxyname, symname);
	    Delete(proxyname);
          } else {
            // global enum or enum in a namespace
	    enumname = Copy(get_c_proxy_name(n));
          }
          Setattr(n, "enumname", enumname);
          Delete(enumname);
        }
      }
    }

    return enumname;
  }


  /* -----------------------------------------------------------------------------
   * substituteResolvedTypeSpecialVariable()
   * ----------------------------------------------------------------------------- */

  void substituteResolvedTypeSpecialVariable(SwigType *classnametype, String *tm, const char *classnamespecialvariable) {
    if (!CPlusPlus) {
      // Just use the original C type when not using C++, we know that this type can be used in the wrappers.
      Clear(tm);
      String* const s = SwigType_str(classnametype, 0);
      Append(tm, s);
      Delete(s);
      return;
    }

    if (SwigType_isenum(classnametype)) {
      String *enumname = getEnumName(enumLookup(classnametype));
      if (enumname)
	Replaceall(tm, classnamespecialvariable, enumname);
      else
	Replaceall(tm, classnamespecialvariable, NewStringf("int"));
    } else {
      scoped_dohptr btype(SwigType_base(classnametype));
      String* typestr = NIL;
      if (current_output == output_wrapper_def || Cmp(btype, "SwigObj") == 0) {
	// Special case, just leave it unchanged.
	typestr = NewString("SwigObj");
      } else {
	typestr = getClassProxyName(classnametype);
	if (!typestr) {
	  if (SwigType_isbuiltin(btype)) {
	    // This should work just as well in C without any changes.
	    typestr = SwigType_str(classnametype, 0);
	  } else {
	    // Swig doesn't know anything about this type, use descriptor for it.
	    typestr = NewStringf("SWIGTYPE%s", SwigType_manglestr(classnametype));

	    // And make sure it is declared before it is used.
	    Printf(sect_wrappers_types, "typedef struct %s %s;\n\n", typestr, typestr);
	  }
	}
      }

      Replaceall(tm, classnamespecialvariable, typestr);
      Delete(typestr);
    }
  }

  /* -----------------------------------------------------------------------------
   * substituteResolvedType()
   *
   * Substitute the special variable $csclassname with the proxy class name for classes/structs/unions
   * that SWIG knows about. Also substitutes enums with enum name.
   * Otherwise use the $descriptor name for the C# class name. Note that the $&csclassname substitution
   * is the same as a $&descriptor substitution, ie one pointer added to descriptor name.
   * Inputs:
   *   pt - parameter type
   *   tm - typemap contents that might contain the special variable to be replaced
   * Outputs:
   *   tm - typemap contents complete with the special variable substitution
   * ----------------------------------------------------------------------------- */

  void substituteResolvedType(SwigType *pt, String *tm) {
    SwigType *type = SwigType_typedef_resolve_all(pt);
    SwigType *strippedtype = SwigType_strip_qualifiers(type);

    if (Strstr(tm, "$resolved_type")) {
      SwigType *classnametype = Copy(strippedtype);
      substituteResolvedTypeSpecialVariable(classnametype, tm, "$resolved_type");
      Delete(classnametype);
    }
    if (Strstr(tm, "$*resolved_type")) {
      SwigType *classnametype = Copy(strippedtype);
      Delete(SwigType_pop(classnametype));
      if (Len(classnametype) > 0) {
	substituteResolvedTypeSpecialVariable(classnametype, tm, "$*resolved_type");
      }
      Delete(classnametype);
    }
    if (Strstr(tm, "$&resolved_type")) {
      SwigType *classnametype = Copy(strippedtype);
      SwigType_add_pointer(classnametype);
      substituteResolvedTypeSpecialVariable(classnametype, tm, "$&resolved_type");
      Delete(classnametype);
    }

    Delete(strippedtype);
    Delete(type);
  }

  /*----------------------------------------------------------------------
   * replaceSpecialVariables()
   *
   * Override the base class method to ensure that $resolved_type is expanded correctly inside $typemap().
   *--------------------------------------------------------------------*/

  virtual void replaceSpecialVariables(String *method, String *tm, Parm *parm) {
    // This function is called by Swig_typemap_lookup(), which may be called when generating C or C++ wrappers, so delegate to the latter one if necessary.
    if (cxx_class_wrapper_ && cxx_class_wrapper_->replaceSpecialVariables(method, tm, parm))
      return;

    SwigType *type = Getattr(parm, "type");
    substituteResolvedType(type, tm);
  }

  /* ------------------------------------------------------------
   * main()
   * ------------------------------------------------------------ */

  virtual void main(int argc, char *argv[]) {
    bool except_flag = CPlusPlus;
    bool use_cxx_wrappers = CPlusPlus;

    // look for certain command line options
    for (int i = 1; i < argc; i++) {
      if (argv[i]) {
        if (strcmp(argv[i], "-help") == 0) {
          Printf(stdout, "%s\n", usage);
        } else if (strcmp(argv[i], "-namespace") == 0) {
	  if (argv[i + 1]) {
	    ns_cxx = NewString(argv[i + 1]);
	    ns_prefix = Swig_string_mangle(ns_cxx);
	    Swig_mark_arg(i);
	    Swig_mark_arg(i + 1);
	    i++;
	  } else {
	    Swig_arg_error();
	  }
        } else if (strcmp(argv[i], "-nocxx") == 0) {
          use_cxx_wrappers = false;
          Swig_mark_arg(i);
        } else if (strcmp(argv[i], "-noexcept") == 0) {
          except_flag = false;
          Swig_mark_arg(i);
        }
      }
    }

    // add a symbol to the parser for conditional compilation
    Preprocessor_define("SWIGC 1", 0);
    if (except_flag)
      Preprocessor_define("SWIG_C_EXCEPT 1", 0);
    if (CPlusPlus)
      Preprocessor_define("SWIG_CPPMODE 1", 0);

    SWIG_library_directory("c");

    // add typemap definitions
    SWIG_typemap_lang("c");
    SWIG_config_file("c.swg");

    String* const ns_prefix_ = ns_prefix ? NewStringf("%s_", ns_prefix) : NewString("");

    // The default naming convention is to use new_Foo(), copy_Foo() and delete_Foo() for the default/copy ctor and dtor of the class Foo, but we prefer to
    // start all Foo methods with the same prefix, so change this. Notice that new/delete are chosen to ensure that we avoid conflicts with the existing class
    // methods, more natural create/destroy, for example, could result in errors if the class already had a method with the same name, but this is impossible
    // for the chosen names as they're keywords in C++ ("copy" is still a problem but we'll just have to live with it).
    Swig_name_register("construct", NewStringf("%s%%n%%c_new", ns_prefix_));
    Swig_name_register("copy", NewStringf("%s%%n%%c_copy", ns_prefix_));
    Swig_name_register("destroy", NewStringf("%s%%n%%c_delete", ns_prefix_));

    // These ones are only needed when using a global prefix, as otherwise the defaults are fine.
    if (ns_prefix) {
      Swig_name_register("member", NewStringf("%s%%n%%c_%%m", ns_prefix_));
      Swig_name_register("type", NewStringf("%s%%c", ns_prefix_));
    }

    Delete(ns_prefix_);

    exceptions_support_ = except_flag ? exceptions_support_enabled : exceptions_support_disabled;

    if (use_cxx_wrappers)
      cxx_wrappers_.initialize();

    allow_overloading();
  }

  /* ---------------------------------------------------------------------
   * top()
   * --------------------------------------------------------------------- */

  virtual int top(Node *n) {
    module_name = Getattr(n, "name");
    String *outfile = Getattr(n, "outfile");

    // initialize I/O
    const scoped_dohptr f_wrappers_cxx(NewFile(outfile, "w", SWIG_output_files()));
    if (!f_wrappers_cxx) {
      FileErrorDisplay(outfile);
      SWIG_exit(EXIT_FAILURE);
    }

    Swig_banner(f_wrappers_cxx);

    // Open the file where all wrapper declarations will be written to in the end.
    outfile_h = Getattr(n, "outfile_h");
    const scoped_dohptr f_wrappers_h(NewFile(outfile_h, "w", SWIG_output_files()));
    if (!f_wrappers_h) {
      FileErrorDisplay(outfile_h);
        SWIG_exit(EXIT_FAILURE);
      }

    Swig_banner(f_wrappers_h);

    // Associate file with the SWIG sections with the same name, so that e.g. "%header" contents end up in sect_header etc.
    const scoped_dohptr sect_begin(NewStringEmpty());
    const scoped_dohptr sect_header(NewStringEmpty());
    const scoped_dohptr sect_runtime(NewStringEmpty());
    const scoped_dohptr sect_init(NewStringEmpty());

    // This one is used outside of this function, so it's a member variable rather than a local one.
    sect_wrappers = NewStringEmpty();

    Swig_register_filebyname("begin", sect_begin);
    Swig_register_filebyname("header", sect_header);
    Swig_register_filebyname("wrapper", sect_wrappers);
    Swig_register_filebyname("runtime", sect_runtime);
    Swig_register_filebyname("init", sect_init);

    // This one is C-specific and goes directly to the output header file.
    Swig_register_filebyname("cheader", f_wrappers_h);

    // Deal with exceptions support.
    if (exceptions_support_ == exceptions_support_enabled) {
      // Redefine SWIG_CException_Raise() to have a unique prefix in the shared library built from SWIG-generated sources to allow using more than one extension
      // in the same process without conflicts. This has to be done in this hackish way because we really need to change the name of the function itself, not
      // its wrapper (which is not even generated).
      Printv(sect_runtime,
	"#define SWIG_CException_Raise ", (ns_prefix ? ns_prefix : module_name), "_SWIG_CException_Raise\n",
	NIL
      );

      // We need to check if we have any %imported modules, as they would already define the exception support code and we want to have exactly one copy of it
      // in the generated shared library, so check for "import" nodes.
      if (find_first_named_import(n)) {
	  // We import another module, which will have already defined SWIG_CException, so set the flag indicating that we shouldn't do it again in this one and
	  // define the symbol to skip compiling its implementation.
	  Printv(sect_runtime, "#define SWIG_CException_DEFINED 1\n", NIL);

	  // Also set a flag telling classDeclaration() to skip creating SWIG_CException wrappers.
	  exceptions_support_ = exceptions_support_imported;
      }
    }

    if (cxx_wrappers_.is_initialized())
      cxx_wrappers_.initialize_exceptions(exceptions_support_);

    {
      String* const include_guard_name = NewStringf("SWIG_%s_WRAP_H_", module_name);
      String* const include_guard_begin = NewStringf(
          "#ifndef %s\n"
          "#define %s\n\n",
          include_guard_name,
          include_guard_name
        );
      String* const include_guard_end = NewStringf(
          "\n"
          "#endif /* %s */\n",
          include_guard_name
        );

      begin_end_output_guard
        include_guard_wrappers_h(f_wrappers_h, include_guard_begin, include_guard_end);

      // All the struct types used by the functions go to f_wrappers_types so that they're certain to be defined before they're used by any functions. All the
      // functions declarations go directly to f_wrappers_decl we write both of them to f_wrappers_h at the end.
      sect_wrappers_types = NewString("");
      sect_wrappers_decl = NewString("");

      {
        cplusplus_output_guard
          cplusplus_guard_wrappers(sect_wrappers),
          cplusplus_guard_wrappers_h(sect_wrappers_decl);

        // emit code for children
        Language::top(n);
      } // close extern "C" guards

      Dump(sect_wrappers_types, f_wrappers_h);
      Delete(sect_wrappers_types);

      Dump(sect_wrappers_decl, f_wrappers_h);
      Delete(sect_wrappers_decl);

      if (cxx_wrappers_.is_initialized()) {
	if (!ns_cxx) {
	  // We need some namespace for the C++ wrappers as otherwise their names could conflict with the C functions, so use the module name if nothing was
	  // explicitly specified.
	  ns_cxx = Copy(module_name);
	}

	Printv(f_wrappers_h, "#ifdef __cplusplus\n\n", NIL);

	// Generate possibly nested namespace declarations, as unfortunately we can't rely on C++17 nested namespace definitions being always available.
	scoped_dohptr cxx_ns_end(NewStringEmpty());
	for (const char* c = Char(ns_cxx);;) {
	  const char* const next = strstr(c, "::");

	  maybe_owned_dohptr ns_component;
	  if (next) {
	    ns_component.assign_owned(NewStringWithSize(c, next - c));
	  } else {
	    ns_component.assign_non_owned((DOH*)c);
	  }

	  Printf(f_wrappers_h, "namespace %s {\n", ns_component.get());
	  Printf(cxx_ns_end, "}\n");

	  if (!next)
	    break;

	  c = next + 2;
	}

	Printv(f_wrappers_h, "\n", NIL);
	Dump(cxx_wrappers_.sect_types, f_wrappers_h);

	Printv(f_wrappers_h, "\n", NIL);
	Dump(cxx_wrappers_.sect_decls, f_wrappers_h);

	Printv(f_wrappers_h, "\n", NIL);
	Dump(cxx_wrappers_.sect_impls, f_wrappers_h);

	Printv(f_wrappers_h, "\n", cxx_ns_end.get(), "\n#endif /* __cplusplus */\n", NIL);
      }
    } // close wrapper header guard

    // write all to the file
    Dump(sect_begin, f_wrappers_cxx);
    Dump(sect_runtime, f_wrappers_cxx);
    Dump(sect_header, f_wrappers_cxx);
    Dump(sect_wrappers, f_wrappers_cxx);
    Dump(sect_init, f_wrappers_cxx);

    return SWIG_OK;
  }

  /* -----------------------------------------------------------------------
   * importDirective()
   * ------------------------------------------------------------------------ */

  virtual int importDirective(Node *n) {
    // When we import another module, we need access to its declarations in our header, so we must include the header generated for that module. Unfortunately
    // there doesn't seem to be any good way to get the name of that header, so we try to guess it from the header name of this module. This is obviously not
    // completely reliable, but works reasonably well in practice and it's not clear what else could we do, short of requiring some C-specific %import attribute
    // specifying the name of the header explicitly.

    // We can only do something if we have a module name.
    if (String* const imported_module_name = Getattr(n, "module")) {
      // Start with our header name.
      scoped_dohptr header_name(Copy(outfile_h));

      // Strip the output directory from the file name, as it should be common to all generated headers.
      Replace(header_name, SWIG_output_directory(), "", DOH_REPLACE_FIRST);

      // And replace our module name with the name of the one being imported.
      Replace(header_name, module_name, imported_module_name, DOH_REPLACE_FIRST);

      // Finally inject inclusion of this header.
      Printv(Swig_filebyname("cheader"), "#include \"", header_name.get(), "\"\n", NIL);
    }

    return Language::importDirective(n);
  }

  /* -----------------------------------------------------------------------
   * globalvariableHandler()
   * ------------------------------------------------------------------------ */

  virtual int globalvariableHandler(Node *n) {
    // Don't export static globals, they won't be accessible when using a shared library, for example.
    if (Checkattr(n, "storage", "static"))
      return SWIG_NOWRAP;

    // We can't export variables defined inside namespaces to C directly, whatever their type, and we can only export them under their original name, so we
    // can't do it when using a global namespace prefix neither.
    if (!ns_prefix && !scoped_dohptr(Swig_scopename_prefix(Getattr(n, "name")))) {
      // If we can export the variable directly, do it, this will be more convenient to use from C code than accessor functions.
      if (String* const var_decl = make_c_var_decl(n)) {
	Printv(sect_wrappers_decl, "SWIGIMPORT ", var_decl, ";\n\n", NIL);
	Delete(var_decl);
	return SWIG_OK;
      }
    }

    // We have to prepend the global prefix to the names of the accessors for this variable, if we use one.
    //
    // Note that we can't just register the name format using the prefix for "get" and "set", as we do it for "member", and using it for both would result in
    // the prefix being used twice for the member variables getters and setters, so we have to work around it here instead.
    if (ns_prefix && !getCurrentClass()) {
      Swig_require("c:globalvariableHandler", n, "*sym:name", NIL);
      Setattr(n, "sym:name", NewStringf("%s_%s", ns_prefix, Getattr(n, "sym:name")));
    }

    // Otherwise, e.g. if it's of a C++-only type, or a reference, generate accessor functions for it.
    int const rc = Language::globalvariableHandler(n);

    if (Getattr(n, "view"))
      Swig_restore(n);

    return rc;
  }

  /* ----------------------------------------------------------------------
   * prepend_feature()
   * ---------------------------------------------------------------------- */

  String* prepend_feature(Node *n) {
    String *prepend_str = Getattr(n, "feature:prepend");
    if (prepend_str) {
      char *t = Char(prepend_str);
      if (*t == '{') {
        Delitem(prepend_str, 0);
        Delitem(prepend_str, DOH_END);
      }
    }
    return (prepend_str ? prepend_str : empty_string);
  }

  /* ----------------------------------------------------------------------
   * append_feature()
   * ---------------------------------------------------------------------- */

  String* append_feature(Node *n) {
    String *append_str = Getattr(n, "feature:append");
    if (append_str) {
      char *t = Char(append_str);
      if (*t == '{') {
        Delitem(append_str, 0);
        Delitem(append_str, DOH_END);
      }
    }
    return (append_str ? append_str : empty_string);
  }

  /* ----------------------------------------------------------------------
   * get_mangled_type()
   * ---------------------------------------------------------------------- */

  String *get_mangled_type(SwigType *type_arg) {
    String *result = NewString("");
    SwigType *type = 0;
    SwigType *tdtype = SwigType_typedef_resolve_all(type_arg);
    if (tdtype)
      type = tdtype;
    else
      type = Copy(type_arg);

    // special cases for ptr to function as an argument
    if (SwigType_ismemberpointer(type)) {
      SwigType_del_memberpointer(type);
      SwigType_add_pointer(type);
    }
    if (SwigType_ispointer(type)) {
      SwigType_del_pointer(type);
      if (SwigType_isfunction(type)) {
        Printf(result, "f");
	Delete(type);
	return result;
      }
      Delete(type);
      type = Copy(type_arg);
    }

    SwigType *prefix = SwigType_prefix(type);
    if (Len(prefix)) {
      Replaceall(prefix, ".", "");
      Replaceall(prefix, "const", "c");
      Replaceall(prefix, "volatile", "v");
      Replaceall(prefix, "a(", "a");
      Replaceall(prefix, "m(", "m");
      Replaceall(prefix, "q(", "");
      Replaceall(prefix, ")", "");
      Replaceall(prefix, " ", "");
      Printf(result, "%s", prefix);
    }

    type = SwigType_base(type);
    if (SwigType_isbuiltin(type)) {
      Printf(result, "%c", *Char(SwigType_base(type)));
    } else if (SwigType_isenum(type)) {
      String* enumname = Swig_scopename_last(type);
      const char* s = Char(enumname);
      static const int len_enum_prefix = strlen("enum ");
      if (strncmp(s, "enum ", len_enum_prefix) == 0)
	s += len_enum_prefix;
      Printf(result, "e%s", s);
    } else {
      Printf(result, "%s", Char(Swig_name_mangle(SwigType_base(type))));
    }

    Delete(prefix);
    Delete(type);

    return result;
  }

  void functionWrapperCSpecific(Node *n)
    {
       // this is C function, we don't apply typemaps to it
       String *name = Getattr(n, "sym:name");
       maybe_owned_dohptr wname = getFunctionWrapperName(n, name);
       SwigType *type = Getattr(n, "type");
       SwigType *return_type = NULL;
       String *arg_names = NULL;
       ParmList *parms = Getattr(n, "parms");
       Parm *p;
       String *proto = NewString("");
       int gencomma = 0;
       bool is_void_return = (SwigType_type(type) == T_VOID);

       // create new function wrapper object
       Wrapper *wrapper = NewWrapper();

       // create new wrapper name
       Setattr(n, "wrap:name", wname); //Necessary to set this attribute? Apparently, it's never read!

       // create function call
       arg_names = Swig_cfunction_call(empty_string, parms);
       if (arg_names) {
            Delitem(arg_names, 0);
            Delitem(arg_names, DOH_END);
       }
       return_type = SwigType_str(type, 0);

       // emit wrapper prototype and code
       for (p = parms, gencomma = 0; p; p = nextSibling(p)) {
            Printv(proto, gencomma ? ", " : "", SwigType_str(Getattr(p, "type"), 0), " ", Getattr(p, "lname"), NIL);
            gencomma = 1;
       }
       Printv(wrapper->def, return_type, " ", wname.get(), "(", proto, ") {\n", NIL);

       // attach 'check' typemaps
       Swig_typemap_attach_parms("check", parms, wrapper);

       // insert constraint checking
       for (p = parms; p; ) {
            String *tm;
            if ((tm = Getattr(p, "tmap:check"))) {
                 Replaceall(tm, "$target", Getattr(p, "lname"));
                 Replaceall(tm, "$name", name);
                 Printv(wrapper->code, tm, "\n", NIL);
                 p = Getattr(p, "tmap:check:next");
            } else {
                 p = nextSibling(p);
            }
       }

       Append(wrapper->code, prepend_feature(n));
       if (!is_void_return) {
            Printv(wrapper->code, return_type, " result;\n", NIL);
            Printf(wrapper->code, "result = ");
       }
       Printv(wrapper->code, Getattr(n, "name"), "(", arg_names, ");\n", NIL);
       Append(wrapper->code, append_feature(n));
       if (!is_void_return)
         Printf(wrapper->code, "return result;\n");
       Printf(wrapper->code, "}");

       Wrapper_print(wrapper, sect_wrappers);

       emit_wrapper_func_decl(n, wname);

       // cleanup
       Delete(proto);
       Delete(arg_names);
       Delete(return_type);
       DelWrapper(wrapper);
    }

  void functionWrapperAppendOverloaded(String *name, Parm* first_param)
    {
       String *over_suffix = NewString("");
       Parm *p;
       String *mangled;

       for (p = first_param; p; p = nextSibling(p)) {
            mangled = get_mangled_type(Getattr(p, "type"));
            Printv(over_suffix, "_", mangled, NIL);
       }
       Append(name, over_suffix);
       Delete(over_suffix);
    }

  scoped_dohptr get_wrapper_func_return_type(Node *n)
    {
      SwigType *type = Getattr(n, "type");
      String *return_type;

      if ((return_type = Swig_typemap_lookup("ctype", n, "", 0))) {
	substituteResolvedType(type, return_type);
      } else {
	Swig_warning(WARN_C_TYPEMAP_CTYPE_UNDEF, input_file, line_number, "No ctype typemap defined for %s\n", SwigType_str(type, 0));
	return_type = NewString("");
      }

      Replaceall(return_type, "::", "_");

      return scoped_dohptr(return_type);
    }

  /* ----------------------------------------------------------------------
   * get_wrapper_func_proto()
   *
   * Return the function signature, i.e. the comma-separated list of argument types and names surrounded by parentheses.
   * If a non-null wrapper is specified, it is used to emit typemap-defined code in it and it also determines whether we're generating the prototype for the
   * declarations or the definitions, which changes the type used for the C++ objects.
   * ---------------------------------------------------------------------- */
  scoped_dohptr get_wrapper_func_proto(Node *n, Wrapper* wrapper = NULL)
    {
       ParmList *parms = Getattr(n, "parms");

       Parm *p;
       String *proto = NewString("(");
       int gencomma = 0;

       // attach the standard typemaps
       if (wrapper) {
	 emit_attach_parmmaps(parms, wrapper);
       } else {
	 // We can't call emit_attach_parmmaps() without a wrapper, it would just crash.
	 // Attach "in" manually, we need it for tmap:in:numinputs below.
	 Swig_typemap_attach_parms("in", parms, 0);
       }
       Setattr(n, "wrap:parms", parms); //never read again?!

       // attach 'ctype' typemaps
       Swig_typemap_attach_parms("ctype", parms, 0);


       // prepare function definition
       for (p = parms, gencomma = 0; p; ) {
            String *tm;
            SwigType *type = NULL;

            while (p && checkAttribute(p, "tmap:in:numinputs", "0")) {
                 p = Getattr(p, "tmap:in:next");
            }
            if (!p) break;

            type = Getattr(p, "type");
            if (SwigType_type(type) == T_VOID) {
                 p = nextSibling(p);
                 continue;
            }

	    if (SwigType_type(type) == T_VARARGS) {
	      Swig_error(Getfile(n), Getline(n), "Vararg function %s not supported.\n", Getattr(n, "name"));
	      return scoped_dohptr(NULL);
	    }

            String *lname = Getattr(p, "lname");
            String *c_parm_type = 0;
            String *arg_name = NewString("");

            Printf(arg_name, "c%s", lname);

	    if ((tm = Getattr(p, "tmap:ctype"))) { // set the appropriate type for parameter
		 c_parm_type = Copy(tm);
		 substituteResolvedType(type, c_parm_type);

		 // We prefer to keep typedefs in the wrapper functions signatures as it makes them more readable, but we can't do it for nested typedefs as
		 // they're not valid in C, so resolve them in this case.
		 if (strstr(Char(c_parm_type), "::")) {
		   SwigType* const tdtype = SwigType_typedef_resolve_all(c_parm_type);
		   Delete(c_parm_type);
		   c_parm_type = tdtype;
		 }

		 // template handling
		 Replaceall(c_parm_type, "$tt", SwigType_lstr(type, 0));
	    } else {
		 Swig_warning(WARN_C_TYPEMAP_CTYPE_UNDEF, input_file, line_number, "No ctype typemap defined for %s\n", SwigType_str(type, 0));
	    }

            Printv(proto, gencomma ? ", " : "", c_parm_type, " ", arg_name, NIL);
            gencomma = 1;

            // apply typemaps for input parameter
            if ((tm = Getattr(p, "tmap:in"))) {
                 Replaceall(tm, "$input", arg_name);
		 if (wrapper) {
		   Setattr(p, "emit:input", arg_name);
		   Printf(wrapper->code, "%s\n", tm);
		 }
                 p = Getattr(p, "tmap:in:next");
            } else {
                 Swig_warning(WARN_TYPEMAP_IN_UNDEF, input_file, line_number, "Unable to use type %s as a function argument.\n", SwigType_str(type, 0));
                 p = nextSibling(p);
            }

            Delete(arg_name);
            Delete(c_parm_type);
       }

       Printv(proto, ")", NIL);
       return scoped_dohptr(proto);
    }

  /* ----------------------------------------------------------------------
   * emit_wrapper_func_decl()
   *
   * Declares the wrapper function, using the C types used for it, in the header.
   * The node here is a function declaration.
   * ---------------------------------------------------------------------- */
  void emit_wrapper_func_decl(Node *n, String *wname)
    {
       current_output = output_wrapper_decl;

       // add function declaration to the proxy header file
       Printv(sect_wrappers_decl, "SWIGIMPORT ", get_wrapper_func_return_type(n).get(), " ", wname, get_wrapper_func_proto(n).get(), ";\n\n", NIL);
    }


    void functionWrapperCPPSpecific(Node *n)
    {
       ParmList *parms = Getattr(n, "parms");
       String *name = Copy(Getattr(n, "sym:name"));

       // mangle name if function is overloaded
       if (Getattr(n, "sym:overloaded")) {
            if (!Getattr(n, "copy_constructor")) {
		Parm* first_param = (Parm*)parms;
		if (first_param) {
		  // Skip the first "this" parameter of the wrapped methods, it doesn't participate in overload resolution and would just result in extra long
		  // and ugly names.
		  //
		  // We need to avoid dropping the first argument of static methods which don't have "this" pointer, in spite of being members (and we have to
		  // use "cplus:staticbase" for this instead of just using Swig_storage_isstatic() because "storage" is reset in staticmemberfunctionHandler()
		  // and so is not available here.
		  //
		  // Of course, the constructors don't have the extra first parameter neither.
		  if (!Checkattr(n, "nodeType", "constructor") &&
			Checkattr(n, "ismember", "1") &&
			  !Getattr(n, "cplus:staticbase")) {
		    first_param = nextSibling(first_param);

		    // A special case of overloading on const/non-const "this" pointer only, we still need to distinguish between those.
		    if (SwigType_isconst(Getattr(n, "decl"))) {
		      const char * const nonconst = Char(Getattr(n, "decl")) + 9 /* strlen("q(const).") */;
		      for (Node* nover = Getattr(n, "sym:overloaded"); nover; nover = Getattr(nover, "sym:nextSibling")) {
			if (nover == n)
			  continue;

			if (Cmp(Getattr(nover, "decl"), nonconst) == 0) {
			  // We have an overload differing by const only, disambiguate.
			  Append(name, "_const");
			  break;
			}
		      }
		    }
		  }

		  functionWrapperAppendOverloaded(name, first_param);
		}
            }
       }

       // make sure lnames are set
       Parm *p;
       int index = 1;
       String *lname = 0;

       for (p = (Parm*)parms, index = 1; p; (p = nextSibling(p)), index++) {
            if(!(lname = Getattr(p, "lname"))) {
                 lname = NewStringf("arg%d", index);
                 Setattr(p, "lname", lname);
            }
       }

       // C++ function wrapper
       current_output = output_wrapper_def;

       SwigType *type = Getattr(n, "type");
       scoped_dohptr return_type = get_wrapper_func_return_type(n);
       maybe_owned_dohptr wname = getFunctionWrapperName(n, name);
       bool is_void_return = (SwigType_type(type) == T_VOID);
       // create new function wrapper object
       Wrapper *wrapper = NewWrapper();

       // create new wrapper name
       Setattr(n, "wrap:name", wname);

       // add variable for holding result of original function 'cppresult'
       if (!is_void_return) {
	 SwigType *value_type = cplus_value_type(type);
	 SwigType* cppresult_type = value_type ? value_type : type;
	 SwigType* ltype = SwigType_ltype(cppresult_type);
	 Wrapper_add_local(wrapper, "cppresult", SwigType_str(ltype, "cppresult"));
	 Delete(ltype);
	 Delete(value_type);
       }

       // create wrapper function prototype
       Printv(wrapper->def, "SWIGEXPORTC ", return_type.get(), " ", wname.get(), NIL);

       Printv(wrapper->def, get_wrapper_func_proto(n, wrapper).get(), NIL);
       Printv(wrapper->def, " {", NIL);

	// emit variables for holding parameters
	emit_parameter_variables(parms, wrapper);

	// emit variable for holding function return value
	emit_return_variable(n, return_type, wrapper);

       // insert constraint checking
       for (p = parms; p; ) {
            String *tm;
            if ((tm = Getattr(p, "tmap:check"))) {
                 Replaceall(tm, "$target", Getattr(p, "lname"));
                 Replaceall(tm, "$name", name);
                 Printv(wrapper->code, tm, "\n", NIL);
                 p = Getattr(p, "tmap:check:next");
            } else {
                 p = nextSibling(p);
            }
       }

       // create action code
       String *action = Getattr(n, "wrap:action");
       if (!action)
         action = NewString("");

       String *cbase_name = Getattr(n, "c:base_name");
       if (cbase_name) {
            Replaceall(action, "arg1)->", NewStringf("(%s*)arg1)->", Getattr(n, "c:inherited_from")));
            Replaceall(action, Getattr(n, "name"), cbase_name);
       }

       Replaceall(action, "result =", "cppresult =");

       // prepare action code to use, e.g. insert try-catch blocks
       action = emit_action(n);

       // emit output typemap if needed
       if (!is_void_return) {
            String *tm;
            if ((tm = Swig_typemap_lookup_out("out", n, "cppresult", wrapper, action))) {
	      // This is ugly, but the type of our result variable is not always the same as the actual return type currently because
	      // get_wrapper_func_return_type() applies ctype typemap to it. These types are more or less compatible though, so we should be able to cast
	      // between them explicitly.
	      const char* start = Char(tm);
	      const char* p = strstr(start, "$result = ");
	      if (p == start || (p && p[-1] == ' ')) {
		Insert(tm, p - start + strlen("$result = "), NewStringf("(%s)", return_type.get()));
	      }
                 Replaceall(tm, "$result", "result");
		 Replaceall(tm, "$owner", GetFlag(n, "feature:new") ? "1" : "0");
                 Printf(wrapper->code, "%s", tm);
                 if (Len(tm))
                   Printf(wrapper->code, "\n");
            } else {
                 Swig_warning(WARN_TYPEMAP_OUT_UNDEF, input_file, line_number, "Unable to use return type %s in function %s.\n", SwigType_str(type, 0), Getattr(n, "name"));
            }
       } else {
            Append(wrapper->code, action);
       }

       // insert cleanup code
       for (p = parms; p; ) {
            String *tm;
            if ((tm = Getattr(p, "tmap:freearg"))) {
                 if (tm && (Len(tm) != 0)) {
                      String *input = NewStringf("c%s", Getattr(p, "lname"));
                      Replaceall(tm, "$source", Getattr(p, "lname"));
                      Replaceall(tm, "$input", input);
                      Delete(input);
                      Printv(wrapper->code, tm, "\n", NIL);
                 }
                 p = Getattr(p, "tmap:freearg:next");
            } else {
                 p = nextSibling(p);
            }
       }

       if (is_void_return) {
	 Replaceall(wrapper->code, "$null", "");
       } else {
	 Replaceall(wrapper->code, "$null", "0");

         Append(wrapper->code, "return result;\n");
       }

       Append(wrapper->code, "}\n");

       Wrapper_print(wrapper, sect_wrappers);

       // cleanup
       DelWrapper(wrapper);

       emit_wrapper_func_decl(n, wname);

       if (cxx_class_wrapper_)
	 cxx_class_wrapper_->emit_member_function(n);

       Delete(name);
    }

  /* ----------------------------------------------------------------------
   * functionWrapper()
   * ---------------------------------------------------------------------- */

  virtual int functionWrapper(Node *n) {
    if (!Getattr(n, "sym:overloaded")) {
      if (!addSymbol(Getattr(n, "sym:name"), n))
	return SWIG_ERROR;
    }

    if (CPlusPlus) {
      functionWrapperCPPSpecific(n);
    } else {
      functionWrapperCSpecific(n);
    }

    return SWIG_OK;
  }

  /* ---------------------------------------------------------------------
   * copy_node()
   *
   * This is not a general-purpose node copying function, but just a helper of classHandler().
   * --------------------------------------------------------------------- */

  Node *copy_node(Node *node) {
    Node *new_node = NewHash();
    Setattr(new_node, "name", Copy(Getattr(node, "name")));
    Setattr(new_node, "ismember", Copy(Getattr(node, "ismember")));
    Setattr(new_node, "view", Copy(Getattr(node, "view")));
    Setattr(new_node, "kind", Copy(Getattr(node, "kind")));
    Setattr(new_node, "access", Copy(Getattr(node, "access")));
    Setattr(new_node, "parms", Copy(Getattr(node, "parms")));
    Setattr(new_node, "type", Copy(Getattr(node, "type")));
    Setattr(new_node, "decl", Copy(Getattr(node, "decl")));

    Node* const parent = parentNode(node);
    Setattr(new_node, "c:inherited_from", Getattr(parent, "name"));
    Setattr(new_node, "sym:name", Getattr(node, "sym:name"));
    Setattr(new_node, "sym:symtab", Getattr(parent, "symtab"));
    set_nodeType(new_node, "cdecl");

    return new_node;
  }

  /* ---------------------------------------------------------------------
   * is_in()
   *
   * tests if given name already exists in one of child nodes of n
   * --------------------------------------------------------------------- */

  Hash *is_in(String *name, Node *n) {
    Hash *h;
    for (h = firstChild(n); h; h = nextSibling(h)) {
      if (Cmp(name, Getattr(h, "name")) == 0)
        return h;
    }
    return 0;
  }

  /* ---------------------------------------------------------------------
   * make_c_var_decl()
   *
   * Return the C declaration for the given node of "variable" kind.
   *
   * If the variable has a type not representable in C, returns NULL, the caller must check for this!
   *
   * This function accounts for two special cases:
   *  1. If the type is an anonymous enum, "int" is used instead.
   *  2. If the type is an array, its bounds are stripped.
   * --------------------------------------------------------------------- */
  String *make_c_var_decl(Node *n) {
    String *name = Getattr(n, "name");
    SwigType *type = Getattr(n, "type");
    String *type_str = SwigType_str(type, 0);

    if (Getattr(n, "unnamedinstance")) {
      // If this is an anonymous enum, we can declare the variable as int even though we can't reference this type.
      if (Strncmp(type_str, "enum $", 6) != 0) {
	// Otherwise we're out of luck, with the current approach of exposing the variables directly we simply can't do it, we would need to use accessor
	// functions instead to support this.
	Swig_error(Getfile(n), Getline(n), "Variables of anonymous non-enum types are not supported.\n");
	return SWIG_ERROR;
      }

      const char * const unnamed_end = strchr(Char(type_str) + 6, '$');
      if (!unnamed_end) {
	Swig_error(Getfile(n), Getline(n), "Unsupported anonymous enum type \"%s\".\n", type_str);
	return SWIG_ERROR;
      }

      String* const int_type_str = NewStringf("int%s", unnamed_end + 1);
      Delete(type_str);
      type_str = int_type_str;
    } else {
      scoped_dohptr btype(SwigType_base(type));
      if (SwigType_isenum(btype)) {
	// Enums are special as they can be unknown, i.e. not wrapped by SWIG. In this case we just use int instead.
	if (!enumLookup(btype)) {
	  Replaceall(type_str, btype, "int");
	}
      } else {
	// Don't bother with checking if type is representable in C if we're wrapping C and not C++ anyhow: of course it is.
	if (CPlusPlus) {
	  if (SwigType_isreference(type))
	    return NIL;

	  if (!SwigType_isbuiltin(btype))
	    return NIL;

	  // Final complication: define bool if it is used here.
	  if (Cmp(btype, "bool") == 0) {
	    Printv(sect_wrappers_types, "#include <stdbool.h>\n\n", NIL);
	  }
	}
      }
    }

    String* const var_decl = NewStringEmpty();
    if (SwigType_isarray(type)) {
      String *dims = Strchr(type_str, '[');
      char *c = Char(type_str);
      c[Len(type_str) - Len(dims) - 1] = '\0';
      Printv(var_decl, c, " ", name, "[]", NIL);
    } else {
      Printv(var_decl, type_str, " ", name, NIL);
    }

    Delete(type_str);

    return var_decl;
  }

  /* ---------------------------------------------------------------------
   * emit_c_struct_def()
   *
   * Append the declarations of C struct members to the given string.
   * Notice that this function has a side effect of outputting all enum declarations inside the struct into sect_wrappers_types directly.
   * This is done to avoid gcc warnings "declaration does not declare anything" given for the anonymous enums inside the structs.
   * --------------------------------------------------------------------- */

  void emit_c_struct_def(String* out, Node *n) {
    for ( Node* node = firstChild(n); node; node = nextSibling(node)) {
      String* const ntype = nodeType(node);
      if (Cmp(ntype, "cdecl") == 0) {
	SwigType* t = NewString(Getattr(node, "type"));
	SwigType_push(t, Getattr(node, "decl"));
	t = SwigType_typedef_resolve_all(t);
	if (SwigType_isfunction(t)) {
            Swig_warning(WARN_C_UNSUPPORTTED, input_file, line_number, "Extending C struct with %s is not currently supported, ignored.\n", SwigType_str(t, 0));
	} else {
	  String* const var_decl = make_c_var_decl(node);
	  Printv(out, cindent, var_decl, ";\n", NIL);
	  Delete(var_decl);
	}
      } else if (Cmp(ntype, "enum") == 0) {
	// This goes directly into sect_wrappers_types, before this struct declaration.
	emit_one(node);
      } else {
	// WARNING: proxy declaration can be different than original code
	if (Cmp(nodeType(node), "extend") == 0)
	  emit_c_struct_def(out, node);
      }
    }
  }

  /* ---------------------------------------------------------------------
   * classDeclaration()
   * --------------------------------------------------------------------- */

  virtual int classDeclaration(Node *n) {
    if (Cmp(Getattr(n, "name"), "SWIG_CException") == 0) {
      // Ignore this class only if it was already wrapped in another module, imported from this one (if exceptions are disabled, we shouldn't be even parsing
      // SWIG_CException in the first place and if they're enabled, we handle it normally).
      if (exceptions_support_ == exceptions_support_imported)
	  return SWIG_NOWRAP;
    }

    return Language::classDeclaration(n);
  }

  /* ---------------------------------------------------------------------
   * classHandler()
   * --------------------------------------------------------------------- */

  virtual int classHandler(Node *n) {
    String* const name = get_c_proxy_name(n);

    if (CPlusPlus) {
      cxx_class_wrapper cxx_class_wrapper_obj(cxx_wrappers_, n);
      temp_ptr_setter<cxx_class_wrapper*> set_cxx_class_wrapper(&cxx_class_wrapper_, &cxx_class_wrapper_obj);

      // inheritance support: attach all members from base classes to this class
      if (List *baselist = Getattr(n, "bases")) {
	Iterator i;
	for (i = First(baselist); i.item; i = Next(i)) {
	  // look for member variables and functions
	  Node *node;
	  for (node = firstChild(i.item); node; node = nextSibling(node)) {
	    if ((Cmp(Getattr(node, "kind"), "variable") == 0)
		|| (Cmp(Getattr(node, "kind"), "function") == 0)) {
	      if ((Cmp(Getattr(node, "access"), "public") == 0)
		  && (Cmp(Getattr(node, "storage"), "static") != 0)) {
		  // Assignment operators are not inherited in C++ and symbols without sym:name should be ignored, not copied into the derived class.
		  if (Getattr(node, "sym:name") && Cmp(Getattr(node, "name"), "operator =") != 0) {
		    String *parent_name = Getattr(parentNode(node), "name");
		    Hash *dupl_name_node = is_in(Getattr(node, "name"), n);
		    // if there's a duplicate inherited name, due to the C++ multiple
		    // inheritance, change both names to avoid ambiguity
		    if (dupl_name_node) {
		      String *cif = Getattr(dupl_name_node, "c:inherited_from");
		      String *old_name = Getattr(dupl_name_node, "sym:name");
		      if (cif && parent_name && (Cmp(cif, parent_name) != 0)) {
			Setattr(dupl_name_node, "sym:name", NewStringf("%s%s", cif ? cif : "", old_name));
			Setattr(dupl_name_node, "c:base_name", old_name);
			Node *new_node = copy_node(node);
			Setattr(new_node, "name", NewStringf("%s%s", parent_name, old_name));
			Setattr(new_node, "c:base_name", old_name);
			appendChild(n, new_node);
		      }
		    } else {
		      appendChild(n, copy_node(node));
		    }
		  }
	      }
	    }
	  }
	}
      }

      // declare type for specific class in the proxy header
      Printv(sect_wrappers_types, "typedef struct SwigObj_", name, " ", name, ";\n\n", NIL);

      return Language::classHandler(n);
    } else {
      // this is C struct, just declare it in the proxy
      String* struct_def = NewStringEmpty();
      String* const tdname = Getattr(n, "tdname");
      if (tdname)
        Append(struct_def, "typedef struct {\n");
      else
        Printv(struct_def, "struct ", name, " {\n", NIL);
      emit_c_struct_def(struct_def, n);
      if (tdname)
        Printv(struct_def, "} ", tdname, ";\n\n", NIL);
      else
        Append(struct_def, "};\n\n");

      Printv(sect_wrappers_types, struct_def, NIL);
      Delete(struct_def);
    }
    return SWIG_OK;
  }

  /* ---------------------------------------------------------------------
   * staticmembervariableHandler()
   * --------------------------------------------------------------------- */

  virtual int staticmembervariableHandler(Node *n) {
    SwigType *type = Getattr(n, "type");
    SwigType *tdtype = SwigType_typedef_resolve_all(type);
    if (tdtype) {
      type = tdtype;
      Setattr(n, "type", type);
    }
    SwigType *btype = SwigType_base(type);
    if (SwigType_isarray(type) && !SwigType_isbuiltin(btype)) {
      // this hack applies to member objects array (not ptrs.)
      SwigType_add_pointer(btype);
      SwigType_add_array(btype, NewStringf("%s", SwigType_array_getdim(type, 0)));
      Setattr(n, "type", btype);
    }
    Delete(type);
    Delete(btype);
    return Language::staticmembervariableHandler(n);
  }

  /* ---------------------------------------------------------------------
   * membervariableHandler()
   * --------------------------------------------------------------------- */

  virtual int membervariableHandler(Node *n) {
    SwigType *type = Getattr(n, "type");
    SwigType *tdtype = SwigType_typedef_resolve_all(type);
    if (tdtype) {
      type = tdtype;
      Setattr(n, "type", type);
    }
    SwigType *btype = SwigType_base(type);
    if (SwigType_isarray(type) && !SwigType_isbuiltin(btype)) {
      // this hack applies to member objects array (not ptrs.)
      SwigType_add_pointer(btype);
      SwigType_add_array(btype, NewStringf("%s", SwigType_array_getdim(type, 0)));
      Setattr(n, "type", btype);
    }
    Delete(type);
    Delete(btype);
    return Language::membervariableHandler(n);
  }

  /* ---------------------------------------------------------------------
   * constructorHandler()
   * --------------------------------------------------------------------- */

  virtual int constructorHandler(Node *n) {
    // For some reason, the base class implementation of constructorDeclaration() only takes care of the copy ctor automatically for the languages not
    // supporting overloading (i.e. not calling allow_overloading(), as we do). So duplicate the relevant part of its code here,
    if (!Abstract && Getattr(n, "copy_constructor")) {
      return Language::copyconstructorHandler(n);
    }

    if (GetFlag(n, "feature:extend")) {
      // Pretend that all ctors added via %extend are overloaded to avoid clash between the functions created for them and the actual exported function, that
      // could have the same "Foo_new" name otherwise.
      SetFlag(n, "sym:overloaded");
    }

    return Language::constructorHandler(n);
  }

  /* ----------------------------------------------------------------------
   * Language::enumforwardDeclaration()
   * ---------------------------------------------------------------------- */

  virtual int enumforwardDeclaration(Node *n) {
    // Base implementation of this function calls enumDeclaration() for "missing" enums, i.e. those without any definition at all. This results in invalid (at
    // least in C++) enum declarations in the output, so simply don't do this here.
    (void) n;
    return SWIG_OK;
  }

  /* ---------------------------------------------------------------------
   * enumDeclaration()
   * --------------------------------------------------------------------- */

  virtual int enumDeclaration(Node *n) {
    if (ImportMode)
      return SWIG_OK;

    if (getCurrentClass() && (cplus_mode != PUBLIC))
      return SWIG_NOWRAP;

    // We don't know here if we're going to have any non-ignored enum elements, so generate enum declaration in a temporary string.
    enum_decl = NewStringEmpty();

    // Another string for C++ enum declaration, which differs from the C one because it never uses the prefix, as C++ enums are declared in the correct scope.
    cxx_enum_decl = cxx_wrappers_.is_initialized() ? NewStringEmpty() : NULL;

    // If we're currently generating a wrapper class, we need an extra level of indent.
    if (cxx_enum_decl) {
      if (cxx_class_wrapper_) {
	cxx_enum_indent = cxx_class_wrapper_->get_indent();
	Append(cxx_enum_decl, cxx_enum_indent);
      } else {
	cxx_enum_indent = "";
      }
    }

    // Preserve the typedef if we have it in the input.
    String* const tdname = Getattr(n, "tdname");
    if (tdname) {
      Printv(enum_decl, "typedef ", NIL);
      if (cxx_enum_decl)
	Printv(cxx_enum_decl, "typedef ", NIL);
    }
    Printv(enum_decl, "enum", NIL);
    if (cxx_enum_decl)
      Printv(cxx_enum_decl, "enum", NIL);

    String* enum_prefix;
    if (Node* const klass = getCurrentClass()) {
      enum_prefix = get_c_proxy_name(klass);
    } else {
      enum_prefix = ns_prefix; // Possibly NULL, but that's fine.
    }

    scoped_dohptr enumname;

    // Unnamed enums may just have no name at all or have a synthesized invalid name of the form "$unnamedN$ which is indicated by "unnamed" attribute.
    //
    // Also note that we use "name" here and not "sym:name" because the latter is the name of typedef if there is one, while we want to use the name of enum
    // itself here and, even more importantly, use the enum, and not the typedef, name as prefix for its elements.
    if (String* const name = Getattr(n, "unnamed") ? NIL : Getattr(n, "name")) {
      // But the name may included the containing class, so get rid of it.
      enumname = Swig_scopename_last(name);

      // C++ enum name shouldn't include the prefix, as this enum is inside a namespace.
      if (cxx_enum_decl)
	Printv(cxx_enum_decl, " ", enumname.get(), NIL);

      if (enum_prefix) {
	enumname = NewStringf("%s_%s", enum_prefix, enumname.get());
      }

      Printv(enum_decl, " ", enumname.get(), NIL);

      // For scoped enums, their name should be prefixed to their elements in addition to any other prefix we use.
      if (Getattr(n, "scopedenum")) {
	enum_prefix = enumname.get();
      }
    }

    enum_prefix_ = enum_prefix ? NewStringf("%s_", enum_prefix) : NewStringEmpty();

    Printv(enum_decl, " {\n", NIL);
    if (cxx_enum_decl)
      Printv(cxx_enum_decl, " {\n", NIL);

    int const len_orig = Len(enum_decl);

    // Emit each enum item.
    Language::enumDeclaration(n);

    // Only emit the enum declaration if there were actually any items.
    if (Len(enum_decl) > len_orig) {
      Printv(enum_decl, "\n}", NIL);
      if (cxx_enum_decl)
	Printv(cxx_enum_decl, "\n", cxx_enum_indent, "}", NIL);

      if (tdname) {
	Printv(enum_decl, " ", enum_prefix_.get(), tdname, NIL);
	if (cxx_enum_decl)
	  Printv(cxx_enum_decl, " ", tdname, NIL);
      }
      Printv(enum_decl, ";\n\n", NIL);
      if (cxx_enum_decl)
	Printv(cxx_enum_decl, ";\n\n", NIL);

      Append(sect_wrappers_types, enum_decl);
      if (cxx_enum_decl) {
	// Enums declared in global scopes can be just defined before everything else, but nested enums have to be defined inside the declaration of the class,
	// which we must be in process of creating, so output them in the appropriate section.
	Append(cxx_class_wrapper_ ? cxx_wrappers_.sect_decls : cxx_wrappers_.sect_types, cxx_enum_decl);
      }
    }

    Delete(enum_decl);
    if (cxx_enum_decl)
      Delete(cxx_enum_decl);

    return SWIG_OK;
  }

  /* ---------------------------------------------------------------------
   * enumvalueDeclaration()
   * --------------------------------------------------------------------- */

  virtual int enumvalueDeclaration(Node *n) {
    if (Cmp(Getattr(n, "ismember"), "1") == 0 && Cmp(Getattr(n, "access"), "public") != 0)
      return SWIG_NOWRAP;
    Swig_require("enumvalueDeclaration", n, "?enumvalueex", "?enumvalue", NIL);

    if (!GetFlag(n, "firstenumitem")) {
      Printv(enum_decl, ",\n", NIL);
      if (cxx_enum_decl)
	Printv(cxx_enum_decl, ",\n", NIL);
    }

    String* const symname = Getattr(n, "sym:name");
    Printv(enum_decl, cindent, enum_prefix_.get(), symname, NIL);
    if (cxx_enum_decl)
      Printv(cxx_enum_decl, cxx_enum_indent, cindent, symname, NIL);

    // We only use "enumvalue", which comes from the input, and not "enumvalueex" synthesized by SWIG itself because C should use the correct value for the enum
    // items without an explicit one anyhow (and "enumvalueex" can't be always used as is in C code for enum elements inside a class or even a namespace).
    String *value = Getattr(n, "enumvalue");
    if (value) {
      // We can't always use the raw value, check its type to see if we need to transform it.
      maybe_owned_dohptr cvalue;
      switch (SwigType_type(Getattr(n, "type"))) {
	case T_BOOL:
	  // Boolean constants can't appear in C code, so replace them with their values in the simplest possible case. This is not exhaustive, of course,
	  // but better than nothing and doing the right thing is not simple at all as we'd need to really parse the expression, just textual substitution wouldn't
	  // be enough (consider e.g. an enum element called "very_true" and another one using it as its value).
	  if (Cmp(value, "true") == 0) {
	    cvalue.assign_owned(NewString("1"));
	  } else if (Cmp(value, "false") == 0) {
	    cvalue.assign_owned(NewString("0"));
	  } else {
	    Swig_error(Getfile(n), Getline(n), "Unsupported boolean enum value \"%s\".\n", value);
	  }
	  break;

	case T_CHAR:
	  // SWIG parser doesn't put single quotes around char values, for some reason, so add them here.
	  cvalue.assign_owned(NewStringf("'%(escape)s'", value));
	  break;

	default:
	  cvalue.assign_non_owned(value);
      }

      Printv(enum_decl, " = ", cvalue.get(), NIL);
      if (cxx_enum_decl)
	Printv(cxx_enum_decl, " = ", cvalue.get(), NIL);
    }

    Swig_restore(n);
    return SWIG_OK;
  }

  /* ---------------------------------------------------------------------
   * constantWrapper()
   * --------------------------------------------------------------------- */

  virtual int constantWrapper(Node *n) {
    String *name = Getattr(n, "sym:name");
    // If it's a #define or a %constant, use raw value and hope that it will work in C as well as in C++. This is not ideal, but using "value" is even worse, as
    // it doesn't even work for simple char constants such as "#define MY_X 'x'", that would end up unquoted in the generated code.
    String *value = Getattr(n, "rawval");

    if (!value) {
      // Check if it's not a static member variable because its "value" is a reference to a C++ variable and won't translate to C correctly.
      //
      // Arguably, those should be handled in overridden memberconstantHandler() and not here.
      value = Getattr(n, "staticmembervariableHandler:value");
      if (value && Equal(Getattr(n, "valuetype"), "char")) {
	// We need to quote this value.
	const unsigned char c = *Char(value);
	Clear(value);
	if (isalnum(c)) {
	  Printf(value, "'%c'", c);
	} else {
	  Printf(value, "'\\x%x%x'", c / 0x10, c % 0x10);
	}
      }
    }

    if (!value) {
      // Fall back on whatever SWIG parsed the value as for all the rest.
      value = Getattr(n, "value");
    }

    Printv(sect_wrappers_decl, "#define ", name, " ", value, "\n", NIL);
    return SWIG_OK;
  }
};				/* class C */

/* -----------------------------------------------------------------------------
 * swig_c()    - Instantiate module
 * ----------------------------------------------------------------------------- */

static Language *new_swig_c() {
  return new C();
}

extern "C" Language *swig_c(void) {
  return new_swig_c();
}

/* -----------------------------------------------------------------------------
 * Static member variables
 * ----------------------------------------------------------------------------- */

const char *C::usage = (char *) "\
C Options (available with -c)\n\
     -namespace ns - use prefix based on the provided namespace\n\
     -nocxx        - do not generate C++ wrappers\n\
     -noexcept     - do not generate exception handling code\n\
\n";

