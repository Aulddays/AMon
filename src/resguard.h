// ResGuard: free some resource upon leaving scope. similar to a unique_ptr with custom deleter
//
// General usage:
//    ResGuard<Res> res = { new Res, del_fun };
// res will act like a Res *,
// and del_fun() will be called upon destruction of res, or res.release() being explicitly called
//
// if del_fun is of type other than void(*)(Res*):
//    ResGuard<Res, int(*)(Res*)> res = { new Res, del_fun };
//
// Specialization for FILE *:
//    ResGuard<FILE> fp1 = fopen(...);
//    FILEGuard fp2 = fopen(...);

#pragma once

#include <stdio.h>

// default deleter types for ResGuard
template <typename T>
struct ResGuardDeleterTrait
{
	typedef void(*type)(T*);
	//typedef std::function<void(T*)> type;
};
template <>
struct ResGuardDeleterTrait<FILE>
{
	typedef decltype(&fclose) type;	// match the prototype of fclose
};

template <typename Res, typename Deleter = typename ResGuardDeleterTrait<Res>::type>
class ResGuard
{
	// Disable copy
	ResGuard(const ResGuard &) = delete;
	ResGuard &operator=(const ResGuard &) = delete;
protected:
	Res *res;
	Deleter deleter;
public:
	// construct with raw pointer and deleter
	ResGuard(Res *r, Deleter d) : res(r), deleter(d) { }
	// move
	ResGuard(ResGuard &&r) { *this = std::move(r); }
	ResGuard &operator=(ResGuard &&r)
	{
		if (this != &r)
		{
			release();
			res = r.res;
			r.res = NULL;
			deleter = std::move(deleter);
		}
		return *this;
	}

	// Default deleters
	// FILE => fclose
	template <typename U = Res, typename std::enable_if<std::is_same<U, FILE>::value, bool>::type = true>
	ResGuard(Res *r) : res(r), deleter(::fclose) { }
	template <typename U = Res, typename std::enable_if<std::is_same<U, FILE>::value, bool>::type = true>
	const ResGuard &operator =(Res *r) { release(); res = r; deleter = fclose; return *this; }

	~ResGuard() { release(); }
	void release() { if (res) deleter(res); res = NULL; }
	operator Res *() const { return res; }
	Res *operator ->() const { return res; }
	Res *get() const { return res; }
	Res &operator *() const { return *res; }
	operator bool() const { return res != NULL; }
};

typedef ResGuard<FILE> FILEGuard;
