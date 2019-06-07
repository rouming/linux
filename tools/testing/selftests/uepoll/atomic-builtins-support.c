// SPDX-License-Identifier: GPL-2.0
/*
 * Just a test to check if gcc supports atomic builtins
 */
unsigned long long v, vv, vvv;

int main(void)
{
	vv = __atomic_load_n(&v, __ATOMIC_ACQUIRE);
	vvv = __atomic_exchange_n(&vv, 0, __ATOMIC_ACQUIRE);

	return __atomic_add_fetch(&vvv, 1, __ATOMIC_RELAXED);
}
