
// DO NOT EDIT THIS FILE - it is machine generated -*- c++ -*-

#ifndef __javax_swing_text_AbstractDocument$LeafElement__
#define __javax_swing_text_AbstractDocument$LeafElement__

#pragma interface

#include <javax/swing/text/AbstractDocument$AbstractElement.h>
extern "Java"
{
  namespace javax
  {
    namespace swing
    {
      namespace text
      {
          class AbstractDocument;
          class AbstractDocument$LeafElement;
          class AttributeSet;
          class Element;
          class Position;
      }
    }
  }
}

class javax::swing::text::AbstractDocument$LeafElement : public ::javax::swing::text::AbstractDocument$AbstractElement
{

public:
  AbstractDocument$LeafElement(::javax::swing::text::AbstractDocument *, ::javax::swing::text::Element *, ::javax::swing::text::AttributeSet *, jint, jint);
  virtual ::java::util::Enumeration * children();
  virtual jboolean getAllowsChildren();
  virtual ::javax::swing::text::Element * getElement(jint);
  virtual jint getElementCount();
  virtual jint getElementIndex(jint);
  virtual jint getEndOffset();
  virtual ::java::lang::String * getName();
  virtual jint getStartOffset();
  virtual jboolean isLeaf();
  virtual ::java::lang::String * toString();
private:
  static const jlong serialVersionUID = -8906306331347768017LL;
  ::javax::swing::text::Position * __attribute__((aligned(__alignof__( ::javax::swing::text::AbstractDocument$AbstractElement)))) startPos;
  ::javax::swing::text::Position * endPos;
public: // actually package-private
  ::javax::swing::text::AbstractDocument * this$0;
public:
  static ::java::lang::Class class$;
};

#endif // __javax_swing_text_AbstractDocument$LeafElement__