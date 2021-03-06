#pragma once

#include <observer/Observable.hpp>
#include <mpc/MpcStereoMixerChannel.hpp>
#include <mpc/MpcIndivFxMixerChannel.hpp>

#include <memory>

namespace mpc {

	class Mpc;

	namespace sampler {

		class Pad
			: public moduru::observer::Observable
		{

		private:
			mpc::Mpc* mpc;

		private:
			int note{ 0 };
			int number{ 0 };
			std::shared_ptr<ctoot::mpc::MpcStereoMixerChannel> stereoMixerChannel{};
			std::shared_ptr<ctoot::mpc::MpcIndivFxMixerChannel> indivFxMixerChannel{};

		public:
			void setNote(int i);
			int getNote();
			std::weak_ptr<ctoot::mpc::MpcStereoMixerChannel> getStereoMixerChannel();
			std::weak_ptr<ctoot::mpc::MpcIndivFxMixerChannel> getIndivFxMixerChannel();
			int getNumber();

		public:
			Pad(mpc::Mpc* mpc, int number);
			~Pad();

		};

	}
}
