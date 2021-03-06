
#ifndef FRIGG_VECTOR_HPP
#define FRIGG_VECTOR_HPP

#include <frigg/macros.hpp>
#include <frigg/cxx-support.hpp>

#include <frigg/traits.hpp>

namespace frigg FRIGG_VISIBILITY {

template<typename T, typename Allocator>
class Vector {
public:
	friend void swap(Vector &a, Vector &b) {
		using std::swap;
		swap(a.p_allocator, b.p_allocator);
		swap(a.p_elements, b.p_elements);
		swap(a.p_size, b.p_size);
		swap(a.p_capacity, b.p_capacity);
	}

	Vector(Allocator &allocator);

	Vector(const Vector &other) = delete;

	Vector(Vector &&other)
	: Vector(*other.p_allocator) {
		swap(*this, other);
	}

	~Vector();

	T &push(const T &element);
	
	T &push(T &&element);

	T &back();

	T pop();

	template<typename... Args>
	void resize(size_t new_size, Args &&... args);

	T *data() {
		return p_elements;
	}

	size_t size() const {
		return p_size;
	}
	
	bool empty() const {
		return size() == 0;
	}

	T *begin() {
		return p_elements;
	}

	const T *begin() const {
		return p_elements;
	}

	T *end() {
		return p_elements + p_size;
	}

	const T *end() const {
		return p_elements + p_size;
	}
	
	const T &operator[] (size_t index) const {
		return p_elements[index];
	}
	T &operator[] (size_t index) {
		return p_elements[index];
	}

private:
	void ensureCapacity(size_t capacity);

	Allocator *p_allocator;
	T *p_elements;
	size_t p_size;
	size_t p_capacity;
};

template<typename T, typename Allocator>
Vector<T, Allocator>::Vector(Allocator &allocator)
		: p_allocator(&allocator), p_elements(nullptr), p_size(0), p_capacity(0) { }

template<typename T, typename Allocator>
Vector<T, Allocator>::~Vector() {
	for(size_t i = 0; i < p_size; i++)
		p_elements[i].~T();
	p_allocator->free(p_elements);
}

template<typename T, typename Allocator>
T &Vector<T, Allocator>::push(const T &element) {
	ensureCapacity(p_size + 1);
	T *pointer = new (&p_elements[p_size]) T(element);
	p_size++;
	return *pointer;
}

template<typename T, typename Allocator>
T &Vector<T, Allocator>::push(T &&element) {
	ensureCapacity(p_size + 1);
	T *pointer = new (&p_elements[p_size]) T(move(element));
	p_size++;
	return *pointer;
}

template<typename T, typename Allocator>
template<typename... Args>
void Vector<T, Allocator>::resize(size_t new_size, Args &&... args) {
	assert(p_size < new_size);
	ensureCapacity(new_size);
	for(size_t i = p_size; i < new_size; i++)
		new (&p_elements[i]) T(forward<Args>(args)...);
	p_size = new_size;
}

template<typename T, typename Allocator>
void Vector<T, Allocator>::ensureCapacity(size_t capacity) {
	if(capacity <= p_capacity)
		return;
	
	size_t new_capacity = capacity * 2;	
	T *new_array = (T *)p_allocator->allocate(sizeof(T) * new_capacity);
	for(size_t i = 0; i < p_capacity; i++)
		new (&new_array[i]) T(move(p_elements[i]));
	
	for(size_t i = 0; i < p_size; i++)
		p_elements[i].~T();
	p_allocator->free(p_elements);

	p_elements = new_array;
	p_capacity = new_capacity;
}

template<typename T, typename Allocator>
T &Vector<T, Allocator>::back() {
	return p_elements[p_size - 1];
}

template<typename T, typename Allocator>
T Vector<T, Allocator>::pop() {
	p_size--;
	T element = move(p_elements[p_size]);
	p_elements[p_size].~T();
	return element;
}

} // namespace frigg

#endif // FRIGG_VECTOR_HPP

