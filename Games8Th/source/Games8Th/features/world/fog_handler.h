#pragma once

// Scene SetupFog (client 0x18027D8B0) - shader params, not env_gradient_fog.
// IDA sub_18027D400: gradient_fog 0x4B01FF63, _2 0x0AA49C2A, _3 0xFBF6448D,
// enable_gradient_fog 0x6E0FAD7E via set_param_i(output+17).
namespace World {
namespace Fog {
	void Shutdown();
	bool ApplyToScene(void* output);
}
}

