/*
This part is responsible for dispensing the rescue packages.
*/

#include "../header/Dispenser.h"

namespace JAFD
{
	namespace Dispenser
	{
		// Set up the Dispenser System
		ReturnCode setup()
		{
			// Do something...

			return ReturnCode::ok;
		}

		// Dispence items
		ReturnCode dispense(uint8_t number)
		{
			// Auswerfen...

			if (/*irgendwas geht schief*/0)
			{
				return ReturnCode::error;
			}

			return ReturnCode::ok;
		}
	}
}