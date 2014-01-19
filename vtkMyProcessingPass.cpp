#include "stdafx.h"

/*=========================================================================

Program:   Visualization Toolkit
Module:    vtkSobelGradientMagnitudePass.cxx

Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
All rights reserved.
See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

This software is distributed WITHOUT ANY WARRANTY; without even
the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkMyProcessingPass.h"

#include "vtkObjectFactory.h"
#include <assert.h>
#include "vtkRenderState.h"
#include "vtkRenderer.h"
#include "vtkgl.h"
#include "vtkFrameBufferObject.h"
#include "vtkTextureObject.h"
#include "vtkShaderProgram2.h"
#include "vtkShader2.h"
#include "vtkShader2Collection.h"
#include "vtkUniformVariables.h"
#include "vtkOpenGLRenderWindow.h"
#include "vtkTextureUnitManager.h"

// to be able to dump intermediate passes into png files for debugging.
// only for vtkSobelGradientMagnitudePass developers.
//#define VTK_SOBEL_PASS_DEBUG

#include "vtkPNGWriter.h"
#include "vtkImageImport.h"
#include "vtkPixelBufferObject.h"
#include "vtkPixelBufferObject.h"
#include "vtkImageExtractComponents.h"

vtkStandardNewMacro(vtkMyProcessingPass);

//extern const char *shader1_fs;

const char *shader1_fs =
"// ============================================================================\n"
"//\n"
"//  Program:   Visualization Toolkit\n"
"//  Module:    vtkSobelGradientMagnitudePassShader1_fs.glsl\n"
"//\n"
"//  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen\n"
"//  All rights reserved.\n"
"//  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.\n"
"//\n"
"//     This software is distributed WITHOUT ANY WARRANTY; without even\n"
"//     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR\n"
"//     PURPOSE.  See the above copyright notice for more information.\n"
"//\n"
"// ============================================================================\n"
"\n"
"// Fragment shader used by the first pass of the Sobel filter render pass.\n"
"\n"
"#version 110\n"
"\n"
"// GLSL Spec 1.10 rev 59 30-April-2004 defines gl_FragData[] but implementation\n"
"// older than the spec only has it as an extension\n"
"// (nVidia Linux driver 100.14.13, OpenGL version 2.1.1,\n"
"// on Quadro FX 3500/PCI/SSE2)\n"
"#extension GL_ARB_draw_buffers : enable\n"
"\n"
"uniform sampler2D source;\n"
"uniform float step; // 1/W\n"
"\n"
"void main(void)\n"
"{\n"
"  vec2 tcoord=gl_TexCoord[0].st;\n"
"  vec2 offset=vec2(step,0.0);\n"
"  vec4 t1=texture2D(source,tcoord-offset);\n"
"  vec4 t2=texture2D(source,tcoord);\n"
"  vec4 t3=texture2D(source,tcoord+offset);\n"
"  \n"
"  // Gx\n"
"  \n"
"  // version with unclamped float textures t3-t1 will be in [-1,1]\n"
"//  gl_FragData[0]=t3-t1;\n"
"  \n"
"  // version with clamped unchar textures (t3-t1+1)/2 stays in [0,1]\n"
"  gl_FragData[0]=(t3-t1+1.0)/2.0;\n"
"  \n"
"  // Gy\n"
"  gl_FragData[1]=(t1+2.0*t2+t3)/4.0;\n"
"}\n"
"\n";

//extern const char *shader2_fs;

const char *shader2_fs =
"// ============================================================================\n"
"//\n"
"//  Program:   Visualization Toolkit\n"
"//  Module:    vtkSobelGradientMagnitudePassShader2_fs.glsl\n"
"//\n"
"//  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen\n"
"//  All rights reserved.\n"
"//  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.\n"
"//\n"
"//     This software is distributed WITHOUT ANY WARRANTY; without even\n"
"//     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR\n"
"//     PURPOSE.  See the above copyright notice for more information.\n"
"//\n"
"// ============================================================================\n"
"\n"
"// Fragment shader used by the second pass of the Sobel filter render pass.\n"
"\n"
"#version 110\n"
"\n"
"uniform sampler2D gx1;\n"
"uniform sampler2D gy1;\n"
"uniform float step; // 1/H\n"
"\n"
"void main(void)\n"
"{\n"
"  vec2 tcoord=gl_TexCoord[0].st;\n"
"  vec2 offset=vec2(0.0,step);\n"
"  \n"
"  // Gx\n"
"  \n"
"  vec4 tx1=texture2D(gx1,tcoord-offset);\n"
"  vec4 tx2=texture2D(gx1,tcoord);\n"
"  vec4 tx3=texture2D(gx1,tcoord+offset);\n"
"  \n"
"  // if clamped textures, rescale values from [0,1] to [-1,1]\n"
"  tx1=tx1*2.0-1.0;\n"
"  tx2=tx2*2.0-1.0;\n"
"  tx3=tx3*2.0-1.0;\n"
"  \n"
"  vec4 gx=(tx1+2.0*tx2+tx3)/4.0;\n"
"  \n"
"  // Gy\n"
"  \n"
"  vec4 ty1=texture2D(gy1,tcoord-offset);\n"
"  vec4 ty3=texture2D(gy1,tcoord+offset);\n"
"  \n"
"  vec4 gy=ty3-ty1;\n"
"  \n"
"  // the maximum gradient magnitude is sqrt(2.0) when for example gx=1 and\n"
"  // gy=1\n"
"//  gl_FragColor=sqrt((gx*gx+gy*gy)/2.0);\n"
"  gl_FragColor.rgb=sqrt((gx.rgb*gx.rgb+gy.rgb*gy.rgb)/2.0);\n"
"  gl_FragColor.a=1.0; // arbitrary choice.\n"
"}\n"
"\n";


// ----------------------------------------------------------------------------
vtkMyProcessingPass::vtkMyProcessingPass()
{
	this->FrameBufferObject = 0;
	this->Pass1 = 0;
	this->Gx1 = 0;
	this->Gy1 = 0;
	this->Program1 = 0;
	this->Program2 = 0;
}

// ----------------------------------------------------------------------------
vtkMyProcessingPass::~vtkMyProcessingPass()
{
	if (this->FrameBufferObject != 0)
	{
		vtkErrorMacro(<< "FrameBufferObject should have been deleted in ReleaseGraphicsResources().");
	}
	if (this->Pass1 != 0)
	{
		vtkErrorMacro(<< "Pass1 should have been deleted in ReleaseGraphicsResources().");
	}
	if (this->Gx1 != 0)
	{
		vtkErrorMacro(<< "Gx1 should have been deleted in ReleaseGraphicsResources().");
	}
	if (this->Gy1 != 0)
	{
		vtkErrorMacro(<< "Gx1 should have been deleted in ReleaseGraphicsResources().");
	}
	if (this->Program1 != 0)
	{
		this->Program1->Delete();
	}
	if (this->Program2 != 0)
	{
		this->Program2->Delete();
	}
}

// ----------------------------------------------------------------------------
void vtkMyProcessingPass::PrintSelf(ostream& os, vtkIndent indent)
{
	this->Superclass::PrintSelf(os, indent);
}

// ----------------------------------------------------------------------------
// Description:
// Perform rendering according to a render state \p s.
// \pre s_exists: s!=0
void vtkMyProcessingPass::Render(const vtkRenderState *s)
{
	assert("pre: s_exists" && s != 0);

	this->NumberOfRenderedProps = 0;

	if (this->DelegatePass != 0)
	{
		vtkRenderer *r = s->GetRenderer();

		// Test for Hardware support. If not supported, just render the delegate.
		bool supported = vtkFrameBufferObject::IsSupported(r->GetRenderWindow());

		if (!supported)
		{
			vtkErrorMacro("FBOs are not supported by the context. Cannot detect edges on the image.");
		}
		if (supported)
		{
			supported = vtkTextureObject::IsSupported(r->GetRenderWindow());
			if (!supported)
			{
				vtkErrorMacro("Texture Objects are not supported by the context. Cannot detect edges on the image.");
			}
		}

		if (supported)
		{
			supported =
				vtkShaderProgram2::IsSupported(static_cast<vtkOpenGLRenderWindow *>(
				r->GetRenderWindow()));
			if (!supported)
			{
				vtkErrorMacro("GLSL is not supported by the context. Cannot detect edges on the image.");
			}
		}

		if (!supported)
		{
			this->DelegatePass->Render(s);
			this->NumberOfRenderedProps +=
				this->DelegatePass->GetNumberOfRenderedProps();
			return;
		}

		GLint savedDrawBuffer;
		glGetIntegerv(GL_DRAW_BUFFER, &savedDrawBuffer);

		// 1. Create a new render state with an FBO.

		int width = 0;
		int height = 0;
		int size[2];
		s->GetWindowSize(size);
		width = size[0];
		height = size[1];

		const int extraPixels = 1; // one on each side

		int w = width + 2 * extraPixels;
		int h = height + 2 * extraPixels;

		if (this->Pass1 == 0)
		{
			this->Pass1 = vtkTextureObject::New();
			this->Pass1->SetContext(r->GetRenderWindow());
		}

		if (this->FrameBufferObject == 0)
		{
			this->FrameBufferObject = vtkFrameBufferObject::New();
			this->FrameBufferObject->SetContext(r->GetRenderWindow());
		}

		this->RenderDelegate(s, width, height, w, h, this->FrameBufferObject,
			this->Pass1);

#ifdef VTK_SOBEL_PASS_DEBUG
		// Save first pass in file for debugging.
		vtkPixelBufferObject *pbo = this->Pass1->Download();

		unsigned char *openglRawData = new unsigned char[4 * w*h];
		unsigned int dims[2];
		dims[0] = w;
		dims[1] = h;
		vtkIdType incs[2];
		incs[0] = 0;
		incs[1] = 0;
		bool status = pbo->Download2D(VTK_UNSIGNED_CHAR, openglRawData, dims, 4, incs);
		assert("check" && status);
		pbo->Delete();

		// no pbo
		this->Pass1->Bind();
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, openglRawData);
		this->Pass1->UnBind();

		vtkImageImport *importer = vtkImageImport::New();
		importer->CopyImportVoidPointer(openglRawData, 4 * w*h*sizeof(unsigned char));
		importer->SetDataScalarTypeToUnsignedChar();
		importer->SetNumberOfScalarComponents(4);
		importer->SetWholeExtent(0, w - 1, 0, h - 1, 0, 0);
		importer->SetDataExtentToWholeExtent();
		delete[] openglRawData;

		vtkImageExtractComponents *rgbaToRgb = vtkImageExtractComponents::New();
		rgbaToRgb->SetInputConnection(importer->GetOutputPort());
		rgbaToRgb->SetComponents(0, 1, 2);

		vtkPNGWriter *writer = vtkPNGWriter::New();
		writer->SetFileName("SobelPass1.png");
		writer->SetInputConnection(rgbaToRgb->GetOutputPort());
		importer->Delete();
		rgbaToRgb->Delete();
		writer->Write();
		writer->Delete();
#endif

		// 3. Same FBO, but two color attachments (new TOs gx1 and gy1).

		if (this->Gx1 == 0)
		{
			this->Gx1 = vtkTextureObject::New();
			this->Gx1->SetContext(this->FrameBufferObject->GetContext());
		}

		if (this->Gx1->GetWidth() != static_cast<unsigned int>(w) ||
			this->Gx1->GetHeight() != static_cast<unsigned int>(h))
		{
			this->Gx1->Create2D(w, h, 4, VTK_UNSIGNED_CHAR, false);
		}

		if (this->Gy1 == 0)
		{
			this->Gy1 = vtkTextureObject::New();
			this->Gy1->SetContext(this->FrameBufferObject->GetContext());
		}

		if (this->Gy1->GetWidth() != static_cast<unsigned int>(w) ||
			this->Gy1->GetHeight() != static_cast<unsigned int>(h))
		{
			this->Gy1->Create2D(w, h, 4, VTK_UNSIGNED_CHAR, false);
		}
#ifdef VTK_SOBEL_PASS_DEBUG
		cout << "gx1 TOid=" << this->Gx1->GetHandle() << endl;
		cout << "gy1 TOid=" << this->Gy1->GetHandle() << endl;
#endif
		this->FrameBufferObject->SetNumberOfRenderTargets(2);
		this->FrameBufferObject->SetColorBuffer(0, this->Gx1);
		this->FrameBufferObject->SetColorBuffer(1, this->Gy1);
		unsigned int indices[2] = { 0, 1 };
		this->FrameBufferObject->SetActiveBuffers(2, indices);

		this->FrameBufferObject->Start(w, h, false);

#ifdef VTK_SOBEL_PASS_DEBUG
		cout << "sobel finish2" << endl;
		glFinish();
#endif

		// Use the horizontal shader to compute the first pass of Gx and Gy.
		// this->Pass1 is the source
		// (this->Gx1 and this->Gy1 are fbo render targets)

		if (this->Program1 == 0)
		{
			this->Program1 = vtkShaderProgram2::New();
			this->Program1->SetContext(
				static_cast<vtkOpenGLRenderWindow *>(
				this->FrameBufferObject->GetContext()));
			vtkShader2 *shader = vtkShader2::New();
			shader->SetType(VTK_SHADER_TYPE_FRAGMENT);
			shader->SetSourceCode(shader1_fs);
			shader->SetContext(this->Program1->GetContext());
			this->Program1->GetShaders()->AddItem(shader);
			shader->Delete();
		}

		this->Program1->Build();

#ifdef VTK_SOBEL_PASS_DEBUG
		this->Program1->PrintActiveUniformVariablesOnCout();
#endif

#ifdef VTK_SOBEL_PASS_DEBUG
		cout << "sobel finish build 1" << endl;
		glFinish();
#endif

		if (this->Program1->GetLastBuildStatus()
			!= VTK_SHADER_PROGRAM2_LINK_SUCCEEDED)
		{
			vtkErrorMacro("Couldn't build the shader program. At this point , it can be an error in a shader or a driver bug.");

			// restore some state.
			this->FrameBufferObject->UnBind();
			glDrawBuffer(savedDrawBuffer);

			return;
		}

		vtkUniformVariables *var = this->Program1->GetUniformVariables();
		vtkTextureUnitManager *tu =
			static_cast<vtkOpenGLRenderWindow *>(r->GetRenderWindow())->GetTextureUnitManager();

		int sourceId = tu->Allocate();
		vtkgl::ActiveTexture(vtkgl::TEXTURE0 + sourceId);
		this->Pass1->Bind();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		var->SetUniformi("source", 1, &sourceId);

		float fvalue = static_cast<float>(1.0 / w);
		var->SetUniformf("step", 1, &fvalue);

		this->Program1->Use();

#ifdef VTK_SOBEL_PASS_DEBUG
		cout << "sobel finish use 1" << endl;
		glFinish();
#endif

#ifdef VTK_SOBEL_PASS_DEBUG
		this->Program1->PrintActiveUniformVariablesOnCout();
#endif

		if (!this->Program1->IsValid())
		{
			vtkErrorMacro(<< this->Program1->GetLastValidateLog());
		}
#ifdef VTK_SOBEL_PASS_DEBUG
		cout << "sobel finish3-" << endl;
		glFinish();
#endif

		this->FrameBufferObject->RenderQuad(0, w - 1, 0, h - 1);

#ifdef VTK_SOBEL_PASS_DEBUG
		cout << "sobel finish3" << endl;
		glFinish();
#endif

		this->Pass1->UnBind();
		tu->Free(sourceId);

#ifdef VTK_SOBEL_PASS_DEBUG

		// Save second pass in file for debugging.
		pbo = this->Gx1->Download();
		openglRawData = new unsigned char[4 * w*h];
		status = pbo->Download2D(VTK_UNSIGNED_CHAR, openglRawData, dims, 4, incs);
		assert("check2" && status);
		pbo->Delete();

		importer = vtkImageImport::New();
		importer->CopyImportVoidPointer(openglRawData, 4 * w*h*sizeof(unsigned char));
		importer->SetDataScalarTypeToUnsignedChar();
		importer->SetNumberOfScalarComponents(4);
		importer->SetWholeExtent(0, w - 1, 0, h - 1, 0, 0);
		importer->SetDataExtentToWholeExtent();
		delete[] openglRawData;

		rgbaToRgb = vtkImageExtractComponents::New();
		rgbaToRgb->SetInputConnection(importer->GetOutputPort());
		rgbaToRgb->SetComponents(0, 1, 2);

		writer = vtkPNGWriter::New();
		writer->SetFileName("SobelPass2Gx1.png");
		writer->SetInputConnection(rgbaToRgb->GetOutputPort());
		importer->Delete();
		rgbaToRgb->Delete();
		writer->Write();
		writer->Delete();

		pbo = this->Gy1->Download();
		openglRawData = new unsigned char[4 * w*h];
		status = pbo->Download2D(VTK_UNSIGNED_CHAR, openglRawData, dims, 4, incs);
		assert("check3" && status);
		pbo->Delete();

		importer = vtkImageImport::New();
		importer->CopyImportVoidPointer(openglRawData, 4 * w*h*sizeof(unsigned char));
		importer->SetDataScalarTypeToUnsignedChar();
		importer->SetNumberOfScalarComponents(4);
		importer->SetWholeExtent(0, w - 1, 0, h - 1, 0, 0);
		importer->SetDataExtentToWholeExtent();
		delete[] openglRawData;

		rgbaToRgb = vtkImageExtractComponents::New();
		rgbaToRgb->SetInputConnection(importer->GetOutputPort());
		rgbaToRgb->SetComponents(0, 1, 2);

		writer = vtkPNGWriter::New();
		writer->SetFileName("SobelPass2Gy1.png");
		writer->SetInputConnection(rgbaToRgb->GetOutputPort());
		importer->Delete();
		rgbaToRgb->Delete();
		writer->Write();
		writer->Delete();

#endif

		// 4. Render in original FB (from renderstate in arg)

		this->Program1->Restore();

		this->FrameBufferObject->UnBind();

		glDrawBuffer(savedDrawBuffer);

		if (this->Program2 == 0)
		{
			this->Program2 = vtkShaderProgram2::New();
			this->Program2->SetContext(
				static_cast<vtkOpenGLRenderWindow *>(
				this->FrameBufferObject->GetContext()));
			vtkShader2 *shader = vtkShader2::New();
			shader->SetType(VTK_SHADER_TYPE_FRAGMENT);
			shader->SetSourceCode(shader2_fs);
			shader->SetContext(this->Program2->GetContext());
			this->Program2->GetShaders()->AddItem(shader);
			shader->Delete();
		}

		this->Program2->Build();

#ifdef VTK_SOBEL_PASS_DEBUG
		this->Program2->PrintActiveUniformVariablesOnCout();
#endif

#ifdef VTK_SOBEL_PASS_DEBUG
		cout << "sobel finish build 2" << endl;
		glFinish();
#endif

		if (this->Program2->GetLastBuildStatus()
			!= VTK_SHADER_PROGRAM2_LINK_SUCCEEDED)
		{
			vtkErrorMacro("Couldn't build the shader program. At this point , it can be an error in a shader or a driver bug.");

			// restore some state.
			vtkgl::ActiveTexture(vtkgl::TEXTURE0);
			return;
		}

		// this->Gx1 and this->Gy1 are the sources
		int id0 = tu->Allocate();
		int id1 = tu->Allocate();

		vtkgl::ActiveTexture(vtkgl::TEXTURE0 + id0);
		this->Gx1->Bind();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		vtkgl::ActiveTexture(vtkgl::TEXTURE0 + id1);
		this->Gy1->Bind();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		var = this->Program2->GetUniformVariables();

		var->SetUniformi("gx1", 1, &id0);
		var->SetUniformi("gy1", 1, &id1);

		fvalue = static_cast<float>(1.0 / h);
		var->SetUniformf("step", 1, &fvalue);

		this->Program2->Use();

#ifdef VTK_SOBEL_PASS_DEBUG
		cout << "sobel finish use 2" << endl;
		glFinish();
#endif

#ifdef VTK_SOBEL_PASS_DEBUG
		this->Program2->PrintActiveUniformVariablesOnCout();
#endif

		if (!this->Program2->IsValid())
		{
			vtkErrorMacro(<< this->Program2->GetLastValidateLog());
		}
#ifdef VTK_SOBEL_PASS_DEBUG
		cout << "sobel finish 4-" << endl;
		glFinish();
#endif

		// Prepare blitting
		glDisable(GL_ALPHA_TEST);
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_LIGHTING);
		glDisable(GL_SCISSOR_TEST);

		// Trigger a draw on Gy1 (could be called on Gx1).
		this->Gy1->CopyToFrameBuffer(extraPixels, extraPixels,
			w - 1 - extraPixels, h - 1 - extraPixels,
			0, 0, width, height);

		this->Gy1->UnBind();
		vtkgl::ActiveTexture(vtkgl::TEXTURE0 + id0);
		this->Gx1->UnBind();
		vtkgl::ActiveTexture(vtkgl::TEXTURE0);

		tu->Free(id1);
		tu->Free(id0);

#ifdef VTK_SOBEL_PASS_DEBUG
		cout << "sobel finish4" << endl;
		glFinish();
#endif

		this->Program2->Restore();
	}
	else
	{
		vtkWarningMacro(<< " no delegate.");
	}
}

// ----------------------------------------------------------------------------
// Description:
// Release graphics resources and ask components to release their own
// resources.
// \pre w_exists: w!=0
void vtkMyProcessingPass::ReleaseGraphicsResources(vtkWindow *w)
{
	assert("pre: w_exists" && w != 0);

	this->Superclass::ReleaseGraphicsResources(w);

	if (this->Program1 != 0)
	{
		this->Program1->ReleaseGraphicsResources();
	}

	if (this->Program2 != 0)
	{
		this->Program2->ReleaseGraphicsResources();
	}

	if (this->FrameBufferObject != 0)
	{
		this->FrameBufferObject->Delete();
		this->FrameBufferObject = 0;
	}

	if (this->Pass1 != 0)
	{
		this->Pass1->Delete();
		this->Pass1 = 0;
	}

	if (this->Gx1 != 0)
	{
		this->Gx1->Delete();
		this->Gx1 = 0;
	}

	if (this->Gy1 != 0)
	{
		this->Gy1->Delete();
		this->Gy1 = 0;
	}
}