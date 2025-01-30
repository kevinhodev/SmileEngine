// Copyright 2012-2021 Crytek GmbH / Crytek Group. All rights reserved.

/*************************************************************************
-------------------------------------------------------------------------
History:
- 12:12:2011		Created by Jonathan Bunner

*************************************************************************/

#ifndef __RANDOMDECK_H__
#define __RANDOMDECK_H__

#include <CryMath/MTPseudoRandom.h>

// Functor object for the stl shuffle algorithm currently used by CRandomDeck
class CRandomIntGenerator
{
public:
	CRandomIntGenerator(){}; 

	void Seed(int seed);

	using result_type = uint32;
	static constexpr result_type min() { return std::numeric_limits<result_type>::lowest(); }
	static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }
	result_type operator()()
	{
		return static_cast<result_type>(m_twisterNumberGen.GenerateUint32());
	}

	// Random number gen based on Mersenne twister 
	CMTRand_int32 m_twisterNumberGen; 
};

// Helper class
// - shuffles a contiguous series of integers in range minIndex->maxIndex
// and hands out a new index when DealNext() is called. Automatically 
// reshuffles when deck emptied
class CRandomNumberDeck
{
public:

	// TODO - could support weightings here too (e.g. adding more of specified indecies to the deck)
	CRandomNumberDeck(); 
	~CRandomNumberDeck(); 

	uint8 DealNext();
	void Init(int seed, uint8 maxIndexValue, uint8 minIndexValue = 0, const bool autoReshuffleOnEmpty = true); 

	void Shuffle(); 
	ILINE bool Empty() {return m_deck.empty();} 
private:

	std::vector<int> m_deck; 
	int m_seed; 
	uint8 m_minIndex; 
	uint8 m_maxIndex;
	bool m_bAutoReshuffleOnEmpty; 

	CRandomIntGenerator m_randomIntGenerator;
};


#endif //#ifndef __RANDOMDECK_H__
