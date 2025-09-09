#nullable enable

using UnityEngine;
using UnityEngine.UI;

namespace Grpc.Sample
{
    static class BorderUtil
    {
        public static void ApplyBorder(RectTransform target, int px, Color color)
        {
            Ensure(target, "Border_Top",  out var top,    color);
            Ensure(target, "Border_Bot",  out var bot,    color);
            Ensure(target, "Border_Left", out var left,   color);
            Ensure(target, "Border_Rght", out var right,  color);

            var sz = target.sizeDelta;

            top.sizeDelta = new Vector2(sz.x, px);
            top.anchoredPosition = new Vector2(0f, sz.y * 0.5f - px * 0.5f);

            bot.sizeDelta = new Vector2(sz.x, px);
            bot.anchoredPosition = new Vector2(0f, -sz.y * 0.5f + px * 0.5f);

            left.sizeDelta = new Vector2(px, sz.y);
            left.anchoredPosition = new Vector2(-sz.x * 0.5f + px * 0.5f, 0f);

            right.sizeDelta = new Vector2(px, sz.y);
            right.anchoredPosition = new Vector2(sz.x * 0.5f - px * 0.5f, 0f);
        }

        static void Ensure(RectTransform parent, string name, out RectTransform rt, Color color)
        {
            Transform child = parent.Find(name);
            GameObject go;
            if (child != null)
            {
                go = child.gameObject;
            }
            else
            {
                go = new GameObject(name);
            }

            if (go.transform.parent != parent) 
                go.transform.SetParent(parent, false);

            rt = go.GetComponent<RectTransform>();
            if (rt == null) rt = go.AddComponent<RectTransform>();
            rt.pivot = new Vector2(0.5f, 0.5f);
            rt.anchorMin = rt.anchorMax = new Vector2(0.5f, 0.5f);

            var img = go.GetComponent<Image>();
            if (img == null) img = go.AddComponent<Image>();
            img.sprite = Sprite.Create(Texture2D.whiteTexture, new Rect(0,0,1,1), new Vector2(0.5f,0.5f));
            img.color = color;
            img.raycastTarget = false;
        }
    }
}