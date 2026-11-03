#include "OKHud.h"

FOKScreenObject& FOKHudOverlay::Add(int32 Id, int32 Layer)
{
	FOKScreenObject& Object = Objects.AddDefaulted_GetRef();
	Object.Id = Id;
	Object.Layer = Layer;
	return Object;
}

FOKScreenObject* FOKHudOverlay::Find(int32 Id)
{
	return Objects.FindByPredicate([Id](const FOKScreenObject& Object) { return Object.Id == Id; });
}

void FOKHudOverlay::Free(int32 Id)
{
	Objects.RemoveAll([Id](const FOKScreenObject& Object) { return Object.Id == Id; });
}

void FOKHudOverlay::ShowBanner(const FOKHudBanner& Banner)
{
	Banners.RemoveAll([&](const FOKHudBanner& Old) { return Old.Until <= Banner.Start || (Old.Kind == Banner.Kind && Old.Side == Banner.Side); });
	// "Fight!" takes over from "Round N"
	if (Banner.Kind == FOKHudBanner::EKind::Fight)
	{
		Banners.RemoveAll([](const FOKHudBanner& Old) { return Old.Kind == FOKHudBanner::EKind::Round; });
	}
	Banners.Add(Banner);
}
